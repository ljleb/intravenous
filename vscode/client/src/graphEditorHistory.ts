import type { WebviewPanel } from "vscode";
import type { GraphEditorHostMessage } from "./graphEditorProtocol";

type HistoryOperation = {
    requestId: number;
    waitingForVisiblePanels: Set<WebviewPanel>;
    finishWaiting: () => void;
};

export class GraphEditorHistory {
    // A suspended panel can stop delaying the current command and still owe
    // an acknowledgment before it can edit again.
    private readonly panelsOwingCleanup = new Map<WebviewPanel, number>();
    private active?: HistoryOperation;
    private lastRequestId = 0;
    private queue = Promise.resolve();
    private disposed = false;

    constructor(private readonly readyPanels: ReadonlySet<WebviewPanel>) {}

    get hasStarted(): boolean { return this.lastRequestId !== 0; }

    allowsNodeEdits(panel: WebviewPanel): boolean {
        return !this.active && !this.panelsOwingCleanup.has(panel);
    }

    run(apply: () => Promise<void>): Promise<void> {
        const operation = this.queue.then(() => this.applyAfterCleanup(apply));
        this.queue = operation.catch(() => {});
        return operation;
    }

    panelReady(panel: WebviewPanel): void {
        if (this.active) this.panelsOwingCleanup.set(panel, this.active.requestId);
        void this.postState(panel);
    }

    panelVisibilityChanged(panel: WebviewPanel): void {
        if (panel.visible) void this.postState(panel);
        else this.stopWaitingFor(panel);
    }

    panelClosed(panel: WebviewPanel): void {
        this.panelsOwingCleanup.delete(panel);
        this.stopWaitingFor(panel);
    }

    acknowledge(panel: WebviewPanel, requestId: number): void {
        if (this.panelsOwingCleanup.get(panel) !== requestId) return;
        this.panelsOwingCleanup.delete(panel);
        this.stopWaitingFor(panel);
        void this.postState(panel);
    }

    dispose(): void {
        this.disposed = true;
        this.panelsOwingCleanup.clear();
        this.active?.finishWaiting();
    }

    private stopWaitingFor(panel: WebviewPanel): void {
        const operation = this.active;
        if (operation?.waitingForVisiblePanels.delete(panel) && !operation.waitingForVisiblePanels.size)
            operation.finishWaiting();
    }

    private async applyAfterCleanup(apply: () => Promise<void>): Promise<void> {
        if (this.disposed) return;
        const requestId = ++this.lastRequestId;
        const waitingForVisiblePanels = new Set<WebviewPanel>();
        for (const panel of this.readyPanels) {
            this.panelsOwingCleanup.set(panel, requestId);
            if (panel.visible) waitingForVisiblePanels.add(panel);
        }
        const visiblePanelWait = new Promise<void>((finishWaiting) => {
            this.active = { requestId, waitingForVisiblePanels, finishWaiting };
        });
        let timer: ReturnType<typeof setTimeout> | undefined;
        try {
            for (const panel of waitingForVisiblePanels) void this.postState(panel);
            if (waitingForVisiblePanels.size) {
                await Promise.race([visiblePanelWait, new Promise<never>((_, reject) => {
                    timer = setTimeout(() => reject(new Error("A graph editor did not acknowledge interrupted edits")), 5000);
                })]);
                clearTimeout(timer);
            }
            if (!this.disposed) await apply();
        } finally {
            clearTimeout(timer);
            this.active = undefined;
            for (const panel of this.readyPanels) void this.postState(panel);
        }
    }

    private async postState(panel: WebviewPanel): Promise<void> {
        if (this.disposed || !panel.visible || !this.readyPanels.has(panel)) return;
        const pending = this.panelsOwingCleanup.get(panel);
        if (pending === undefined && (this.active || !this.hasStarted)) return;
        const message: GraphEditorHostMessage = pending === undefined
            ? { type: "projectHistoryFinished", requestId: this.lastRequestId }
            : { type: "interruptProjectHistory", requestId: pending };
        // Only the handler's acknowledgment confirms cleanup. Retry when a
        // webview that missed the request becomes visible or ready again.
        try { await panel.webview.postMessage(message); } catch {}
    }
}

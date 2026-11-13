import * as crypto from "node:crypto";
import * as vscode from "vscode";
import { GraphLayoutStore } from "./graphLayoutStore";
import { readLayout, type GraphScope } from "./graphScope";
import type { GraphEditorState, GraphEditorWebviewMessage } from "./graphEditorProtocol";
import type { WorkspaceSession } from "./workspaceSession";

export class GraphEditorProvider implements vscode.Disposable {
    private readonly panels = new Set<vscode.WebviewPanel>();
    private readonly ready = new Set<vscode.WebviewPanel>();
    private readonly store: GraphLayoutStore;
    private readonly layoutLoaded: Promise<void>;
    private readonly changed: vscode.Disposable;
    private state: GraphEditorState = {
        sessionStatus: "starting", scope: null, layout: readLayout(null), error: "",
    };
    private dirty = false;
    private reading = false;
    private disposed = false;
    private layoutError = "";

    constructor(private readonly extensionUri: vscode.Uri,
        private readonly session: WorkspaceSession) {
        this.store = new GraphLayoutStore(session.workspaceRoot());
        this.layoutLoaded = this.store.load().then((layout) => { this.state.layout = layout; })
            .catch((error) => {
                this.layoutError = `Cannot read graph layout: ${error.message}. Positions will not be saved until the file is repaired and the window reloaded.`;
                this.state.error = this.layoutError;
            });
        this.changed = session.onDidChangeGraph(() => { void this.refresh(); });
    }

    open(): void {
        const panel = vscode.window.createWebviewPanel("intravenous.graphEditor",
            "Intravenous Graph", vscode.ViewColumn.Active, { enableScripts: true, retainContextWhenHidden: true });
        this.attach(panel);
    }

    // VS Code restores each tab independently after a window reload.
    revive(panel: vscode.WebviewPanel): void { this.attach(panel); }

    private attach(panel: vscode.WebviewPanel): void {
        this.panels.add(panel);
        const root = vscode.Uri.joinPath(this.extensionUri, "dist", "webview");
        panel.webview.options = { enableScripts: true, localResourceRoots: [root] };
        const messages = panel.webview.onDidReceiveMessage(async (message: GraphEditorWebviewMessage) => {
            if (message?.type === "graphReady") {
                await this.layoutLoaded;
                if (!this.panels.has(panel)) return;
                this.ready.add(panel);
                this.post(panel);
                void this.refresh();
            } else if (message?.type === "saveGraphLayout" && message.layout) {
                await this.layoutLoaded;
                if (this.layoutError) return;
                const next = readLayout(message.layout);
                this.state.layout = { ...next, positions: { ...this.state.layout.positions, ...next.positions } };
                try { await this.store.save(this.state.layout); }
                catch (error) {
                    void vscode.window.showErrorMessage(`Cannot save graph layout: ${error.message}`);
                }
            }
        });
        panel.onDidDispose(() => { messages.dispose(); this.panels.delete(panel); this.ready.delete(panel); });
        const nonce = crypto.randomBytes(16).toString("base64");
        const script = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.js"));
        const css = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.css"));
        panel.webview.html = `<!doctype html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${panel.webview.cspSource} data:; style-src ${panel.webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}'">
<link rel="stylesheet" href="${css}"></head><body data-surface="graphEditor"><div id="app"></div>
<script type="module" nonce="${nonce}" src="${script}"></script></body></html>`;
    }

    private post(panel: vscode.WebviewPanel): void {
        if (this.ready.has(panel)) void panel.webview.postMessage({ type: "graphState", state: this.state });
    }

    private async refresh(): Promise<void> {
        if (!this.panels.size || this.disposed) return;
        this.dirty = true;
        if (this.reading) return;
        this.reading = true;
        try {
            await this.layoutLoaded;
            while (this.dirty && !this.disposed) {
                this.dirty = false;
                this.state.sessionStatus = this.session.graphSessionStatus;
                if (this.state.sessionStatus !== "running") {
                    this.state.scope = null;
                } else {
                    try {
                        const scope: GraphScope = await this.session.readGraphScope();
                        // A change while reading requires another complete read.
                        // Do not show a stale response, especially after restart.
                        if (this.dirty || this.disposed) continue;
                        this.state.scope = scope;
                        this.state.error = this.layoutError;
                    } catch (error) {
                        if (this.dirty) continue;
                        this.state.error = `Cannot read graph: ${error.message}`;
                    }
                }
                for (const panel of this.panels) this.post(panel);
            }
        } finally { this.reading = false; }
    }

    dispose(): void {
        this.disposed = true;
        this.changed.dispose();
        for (const panel of this.panels) panel.dispose();
        this.panels.clear();
    }
}

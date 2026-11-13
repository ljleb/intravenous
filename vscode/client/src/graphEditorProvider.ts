import * as crypto from "node:crypto";
import * as vscode from "vscode";
import { GraphLayoutStore } from "./graphLayoutStore";
import { addressKey, readLayout, type GraphScope } from "./graphScope";
import { nodeDebugDetails } from "./nodeDebugDetails";
import type { GraphEditorAction, GraphEditorState, GraphEditorWebviewMessage } from "./graphEditorProtocol";
import type { WorkspaceSession } from "./workspaceSession";

export class GraphEditorProvider implements vscode.Disposable {
    private readonly panels = new Set<vscode.WebviewPanel>();
    private readonly ready = new Set<vscode.WebviewPanel>();
    private lastActivePanel: vscode.WebviewPanel | null = null;
    private readonly pendingActions = new Map<vscode.WebviewPanel, GraphEditorAction[]>();
    private readonly store: GraphLayoutStore;
    private readonly layoutLoaded: Promise<void>;
    private readonly changed: vscode.Disposable;
    private state: GraphEditorState = {
        sessionStatus: "starting", scope: null, layout: readLayout(null), definitions: [], error: "",
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

    open(): vscode.WebviewPanel {
        const panel = vscode.window.createWebviewPanel("intravenous.graphEditor",
            "Intravenous Graph", vscode.ViewColumn.Active, { enableScripts: true, retainContextWhenHidden: true });
        this.attach(panel);
        return panel;
    }

    async runAction(action: GraphEditorAction, target?: vscode.WebviewPanel): Promise<void> {
        const panel = target ?? (this.lastActivePanel && this.panels.has(this.lastActivePanel)
            ? this.lastActivePanel : [...this.panels].find((entry) => entry.active) ?? [...this.panels][0] ?? this.open());
        panel.reveal(panel.viewColumn, false);
        if (!this.ready.has(panel)) {
            this.pendingActions.set(panel, [...(this.pendingActions.get(panel) ?? []), action]);
            return;
        }
        const [scope, definitions] = await Promise.all([this.session.readGraphScope(), this.session.graphDefinitions()]);
        if (!this.panels.has(panel)) return;
        this.state = { ...this.state, scope, definitions, sessionStatus: this.session.graphSessionStatus };
        this.post(panel);
        if (action === "addNode" || action === "createPackage") {
            void panel.webview.postMessage({ type: "graphAction", action });
            return;
        }
        if (action === "deleteConnection") {
            const label = (matcher: GraphScope["connections"][number]["connection"]["outputs"][number]) => {
                const instance = scope.nodes.find((node) => node.instance.instanceId === matcher.instanceId)?.instance;
                return `${instance?.displayName || instance?.definitionId.split(".").pop() || "Unavailable node"}.${matcher.port.name ?? matcher.port.index ?? "*"}`;
            };
            const selected = await vscode.window.showQuickPick(scope.connections.map((entry) => ({
                label: `${entry.connection.outputs.map(label).join(", ")} → ${entry.connection.inputs.map(label).join(", ")}`,
                description: entry.applied ? "" : "Not applied", id: entry.id,
            })), { title: "Intravenous: Delete Connection", placeHolder: "Select a connection" });
            if (selected) { await this.session.deleteGraphConnection(selected.id); await this.refresh(); }
            return;
        }
        const selected = await vscode.window.showQuickPick(scope.nodes.map((node, index) => ({
            label: node.instance.displayName || node.instance.definitionId.split(".").pop() || node.instance.definitionId,
            description: node.instance.definitionId, instanceId: node.instance.instanceId,
            detail: `Lane ${index + 1}${node.instance.configurationSource ? ` · ${node.instance.configurationSource}` : ""}`,
        })), { title: `Intravenous: ${action === "editNode" ? "Edit Node" : action === "deleteNode" ? "Delete Node" : "Open Node Source"}`,
            placeHolder: "Select a node" });
        if (!selected || !this.panels.has(panel)) return;
        if (action === "editNode") {
            const current = await this.session.readGraphScope();
            if (!current.nodes.some((node) => node.instance.instanceId === selected.instanceId)) {
                throw new Error("The selected node is no longer available");
            }
            this.state.scope = current;
            this.post(panel);
            void panel.webview.postMessage({ type: "graphAction", action, instanceId: selected.instanceId });
        } else if (action === "deleteNode") {
            await this.session.deleteGraphNode(selected.instanceId);
            await this.refresh();
        } else {
            await this.session.openGraphNodeSource(selected.instanceId);
        }
    }

    // VS Code restores each tab independently after a window reload.
    revive(panel: vscode.WebviewPanel): void { this.attach(panel); }

    private attach(panel: vscode.WebviewPanel): void {
        this.panels.add(panel);
        if (panel.active) this.lastActivePanel = panel;
        const viewState = panel.onDidChangeViewState(() => {
            if (panel.active) this.lastActivePanel = panel;
        });
        const root = vscode.Uri.joinPath(this.extensionUri, "dist", "webview");
        panel.webview.options = { enableScripts: true, localResourceRoots: [root] };
        const messages = panel.webview.onDidReceiveMessage(async (message: GraphEditorWebviewMessage) => {
            if (message?.type === "graphReady") {
                await this.layoutLoaded;
                if (!this.panels.has(panel)) return;
                this.ready.add(panel);
                this.post(panel);
                await this.refresh();
                const pending = this.pendingActions.get(panel) ?? [];
                this.pendingActions.delete(panel);
                for (const action of pending) {
                    try { await this.runAction(action, panel); }
                    catch (error) { void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`); }
                }
            } else if (message?.type === "copyNodeDebugDetails") {
                const scope = this.state.scope;
                const node = scope?.nodes.find((entry) => addressKey(entry.address) === message.nodeKey);
                if (!scope || !node) return;
                try { await vscode.env.clipboard.writeText(nodeDebugDetails(scope, node)); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot copy node details: ${error.message}`); }
            } else if (message?.type === "connectGraphPorts" || message?.type === "editGraphConnectionEnd") {
                try {
                    if (message.type === "connectGraphPorts") {
                        await this.session.connectGraphPorts(crypto.randomUUID(), message.first, message.second);
                    } else await this.session.editGraphConnectionEnd(message.edit);
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit connection: ${error.message}`);
                }
            } else if (message?.type === "createGraphNode" || message?.type === "updateGraphNode"
                || message?.type === "createGraphPackage") {
                try {
                    if (message.type === "createGraphNode") {
                        await this.session.createGraphNode(message.definitionId, message.displayName, message.configurationSource);
                    } else if (message.type === "updateGraphNode") {
                        await this.session.updateGraphNode(message.instanceId, message.displayName, message.configurationSource,
                            message.previousDisplayName, message.previousConfigurationSource);
                    } else {
                        await this.session.createGraphPackage(message.name);
                    }
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`);
                }
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
        panel.onDidDispose(() => {
            messages.dispose(); viewState.dispose(); this.panels.delete(panel); this.ready.delete(panel);
            this.pendingActions.delete(panel);
            if (this.lastActivePanel === panel) this.lastActivePanel = null;
        });
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
                        const [scope, definitions]: [GraphScope, string[]] = await Promise.all([
                            this.session.readGraphScope(), this.session.graphDefinitions(),
                        ]);
                        // A change while reading requires another complete read.
                        // Do not show a stale response, especially after restart.
                        if (this.dirty || this.disposed) continue;
                        this.state.scope = scope;
                        this.state.definitions = definitions;
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

import * as crypto from "crypto";
import * as vscode from "vscode";

import { VirtualNode } from "./graphModel";
import {
    LiveGraphInstanceRecord,
    liveGraphInstances,
    upsertVirtualNodes,
} from "./liveGraphModel";
import {
    isLiveGraphWebviewMessage,
    LiveGraphControlHandler,
    LiveGraphSessionStatus,
    LiveGraphStateMessage,
    LiveGraphViewState,
} from "./liveGraphProtocol";

// Hosts the Vue live-graph webview (built into dist/webview). The host owns
// the view state and publishes all of it whenever it changes; presentation
// selection and rendering belong to the webview.
export class LiveGraphViewProvider implements vscode.WebviewViewProvider {
    private readonly webviewRoot: vscode.Uri;
    private view: vscode.WebviewView | null = null;
    private webviewReady = false;
    private postScheduled = false;
    private controlHandler: LiveGraphControlHandler | null = null;
    private readonly state: LiveGraphViewState = {
        sessionStatus: "starting",
        instances: [],
        selectedInstanceId: null,
        packageRoot: null,
        nodes: [],
    };

    constructor(extensionUri: vscode.Uri) {
        this.webviewRoot = vscode.Uri.joinPath(extensionUri, "dist", "webview");
    }

    setSessionStatus(status: LiveGraphSessionStatus): void {
        this.state.sessionStatus = status;
        this.schedulePost();
    }

    setInstances(records: LiveGraphInstanceRecord[]): void {
        this.state.instances = liveGraphInstances(Array.isArray(records) ? records : []);
        const validInstanceIds = new Set(this.state.instances.map((instance) => instance.instanceId));
        this.state.nodes = this.state.nodes.filter(
            (node) => !node.instanceId || validInstanceIds.has(node.instanceId));
        if (this.state.selectedInstanceId && !validInstanceIds.has(this.state.selectedInstanceId)) {
            this.state.selectedInstanceId = null;
        }
        this.schedulePost();
    }

    setSelectedInstanceId(instanceId: string | null): void {
        this.state.selectedInstanceId = instanceId;
        this.schedulePost();
    }

    setPackageRoot(packageRoot: string | null): void {
        this.state.packageRoot = packageRoot;
        this.schedulePost();
    }

    setNodes(nodes: VirtualNode[]): void {
        this.state.nodes = Array.isArray(nodes) ? nodes : [];
        this.schedulePost();
    }

    upsertNodes(nodes: VirtualNode[], replaceInstanceIds: string[] = []): void {
        this.state.nodes = upsertVirtualNodes(this.state.nodes, nodes, replaceInstanceIds);
        this.schedulePost();
    }

    setControlHandler(handler: LiveGraphControlHandler): void {
        this.controlHandler = handler;
    }

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        this.webviewReady = false;
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [this.webviewRoot],
        };
        webviewView.webview.onDidReceiveMessage(async (message) => {
            if (!isLiveGraphWebviewMessage(message)) {
                return;
            }
            if (message.type === "ready") {
                // The webview (re)loaded; it has no state until it receives
                // this snapshot.
                if (this.view === webviewView) {
                    this.webviewReady = true;
                    this.postState();
                }
                return;
            }
            if (message.type === "selectInstance") {
                this.state.selectedInstanceId = message.instanceId ?? null;
                this.schedulePost();
            }
            if (this.controlHandler) {
                await this.controlHandler(message);
            }
        });
        webviewView.onDidDispose(() => {
            if (this.view === webviewView) {
                this.view = null;
                this.webviewReady = false;
            }
        });
        webviewView.webview.html = this.getHtml(webviewView.webview);
    }

    // Coalesces the state changes made by one synchronous update into one
    // message.
    private schedulePost(): void {
        if (!this.view || !this.webviewReady || this.postScheduled) {
            return;
        }
        this.postScheduled = true;
        queueMicrotask(() => {
            this.postScheduled = false;
            this.postState();
        });
    }

    private postState(): void {
        if (!this.view || !this.webviewReady) {
            return;
        }
        const message: LiveGraphStateMessage = {
            type: "state",
            state: this.state,
        };
        void this.view.webview.postMessage(message);
    }

    private getHtml(webview: vscode.Webview): string {
        const nonce = crypto.randomBytes(16).toString("base64");
        const scriptUri = webview.asWebviewUri(vscode.Uri.joinPath(this.webviewRoot, "main.js"));
        const styleUri = webview.asWebviewUri(vscode.Uri.joinPath(this.webviewRoot, "main.css"));
        const csp = [
            "default-src 'none'",
            `img-src ${webview.cspSource} data:`,
            `style-src ${webview.cspSource} 'unsafe-inline'`,
            `font-src ${webview.cspSource}`,
            `script-src 'nonce-${nonce}'`,
        ].join("; ");
        return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="Content-Security-Policy" content="${csp}">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="${styleUri}">
</head>
<body>
    <div id="app"></div>
    <script type="module" nonce="${nonce}" src="${scriptUri}"></script>
</body>
</html>`;
    }
}

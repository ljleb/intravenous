const vscode = require("vscode");
const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const net = require("net");
const os = require("os");
const path = require("path");
const mergedNodeIconPath = path.join(__dirname, "media", "merged_node.svg");
const singleNodeIconPath = path.join(__dirname, "media", "single_node.svg");
const arrowRightIconPath = path.join(__dirname, "media", "arrow_right.svg");
const chevronRightSourceIconPath = path.join(__dirname, "media", "chevron_left.svg");
const chevronDownIconPath = path.join(__dirname, "media", "chevron_down.svg");
const portIconSizePx = 16;

class JsonRpcSocketClient {
    constructor(socketPath, notificationHandler = null) {
        this.socketPath = socketPath;
        this.notificationHandler = notificationHandler;
        this.socket = null;
        this.nextId = 1;
        this.pending = new Map();
        this.buffer = "";
    }

    async connect(timeoutMs = 10000) {
        if (this.socket) {
            return;
        }

        await new Promise((resolve, reject) => {
            const socket = net.createConnection(this.socketPath);
            const timeout = setTimeout(() => {
                socket.destroy();
                reject(new Error(`timed out connecting to ${this.socketPath}`));
            }, timeoutMs);

            socket.on("connect", () => {
                clearTimeout(timeout);
                this.socket = socket;
                socket.setEncoding("utf8");
                socket.on("data", (chunk) => this.onData(chunk));
                socket.on("error", (error) => this.failPending(error));
                socket.on("close", () => this.failPending(new Error("Intravenous server connection closed")));
                resolve();
            });
            socket.on("error", (error) => {
                clearTimeout(timeout);
                reject(error);
            });
        });
    }

    async request(method, params) {
        const id = this.nextId++;
        const payload = JSON.stringify({
            jsonrpc: "2.0",
            id,
            method,
            params,
        }) + "\n";

        return await new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.socket.write(payload, "utf8");
        });
    }

    onData(chunk) {
        this.buffer += chunk;
        for (;;) {
            const newline = this.buffer.indexOf("\n");
            if (newline === -1) {
                return;
            }

            const line = this.buffer.slice(0, newline).trim();
            this.buffer = this.buffer.slice(newline + 1);
            if (!line) {
                continue;
            }

            let message;
            try {
                message = JSON.parse(line);
            } catch (error) {
                this.failPending(new Error(`failed to parse server response: ${error.message}`));
                return;
            }

            if (typeof message.id !== "number") {
                if (typeof message.method === "string" && this.notificationHandler) {
                    this.notificationHandler(message.method, message.params || {});
                }
                continue;
            }

            const pending = this.pending.get(message.id);
            if (!pending) {
                continue;
            }
            this.pending.delete(message.id);

            if (message.error) {
                pending.reject(new Error(message.error.message || "unknown JSON-RPC error"));
            } else {
                pending.resolve(message.result);
            }
        }
    }

    failPending(error) {
        for (const { reject } of this.pending.values()) {
            reject(error);
        }
        this.pending.clear();
    }

    dispose() {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
        this.failPending(new Error("Intravenous client disposed"));
    }
}

class LiveGraphViewProvider {
    constructor(extensionUri) {
        this.extensionUri = extensionUri;
        this.view = null;
        this.nodes = [];
    }

    setNodes(nodes) {
        this.nodes = Array.isArray(nodes) ? nodes : [];
        this.postState();
    }

    pruneDeletedNodeState(_deletedNodeIds) {
    }

    logicalIdentitySummary(node) {
        const identity = typeof node?.sourceIdentity === "string" && node.sourceIdentity.length > 0
            ? node.sourceIdentity
            : (typeof node?.id === "string" ? node.id : "");
        if (!identity) {
            return "";
        }

        const matches = [...identity.matchAll(/@([^@#:$]+)$/g)];
        if (matches.length > 0 && matches[0][1]) {
            return matches[0][1];
        }

        const atIndex = identity.lastIndexOf("@");
        if (atIndex >= 0 && atIndex + 1 < identity.length) {
            return identity.slice(atIndex + 1);
        }

        return identity;
    }

    serializeNodes() {
        return this.nodes.map((node) => ({
            id: node.id || "",
            kind: node.kind || node.id || "",
            description: node.memberCount > 1
                ? (() => {
                    const identity = this.logicalIdentitySummary(node);
                    return identity ? `${identity} • ${node.memberCount} nodes` : `${node.memberCount} nodes`;
                })()
                : (this.logicalIdentitySummary(node) || node.id || ""),
            tooltip: `${node.kind || "node"}${this.logicalIdentitySummary(node) ? ` • ${this.logicalIdentitySummary(node)}` : ""}${node.memberCount > 1 ? ` • ${node.memberCount} members` : ""}`,
            memberCount: Number(node.memberCount || 0),
            icon: node.memberCount > 1 ? "merged" : "single",
            groups: [
                this.makePortGroup("sample inputs", node.sampleInputs || [], "input", "sample"),
                this.makePortGroup("sample outputs", node.sampleOutputs || [], "output", "sample"),
                this.makePortGroup("event inputs", node.eventInputs || [], "input", "event"),
                this.makePortGroup("event outputs", node.eventOutputs || [], "output", "event"),
            ].filter(Boolean),
        }));
    }

    makePortGroup(label, ports, direction, portKind) {
        if (!Array.isArray(ports) || ports.length === 0) {
            return null;
        }
        return {
            label,
            count: ports.length,
            direction,
            portKind,
            ports: ports.map((port, index) => ({
                name: port.name || `[${index}]`,
                connectivity: port.connectivity || "disconnected",
            })),
        };
    }

    resolveWebviewView(webviewView) {
        this.view = webviewView;
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, "media")],
        };
        webviewView.webview.html = this.getHtml(webviewView.webview);
        this.postState();
    }

    postState() {
        if (!this.view) {
            return;
        }
        this.view.webview.postMessage({
            type: "setState",
            nodes: this.serializeNodes(),
        });
    }

    getHtml(webview) {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
        const mergedIconUri = webview.asWebviewUri(vscode.Uri.file(mergedNodeIconPath));
        const singleIconUri = webview.asWebviewUri(vscode.Uri.file(singleNodeIconPath));
        const arrowRightSvg = fs.readFileSync(arrowRightIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');
        const chevronRightSvg = fs.readFileSync(chevronRightSourceIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');
        const chevronDownSvg = fs.readFileSync(chevronDownIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');

        return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource} data:; style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}';">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        :root {
            color-scheme: light dark;
        }

        body {
            margin: 0;
            padding: 0;
            background: var(--vscode-sideBar-background);
            color: var(--vscode-foreground);
            font-family: var(--vscode-font-family);
            font-size: var(--vscode-font-size);
            font-weight: var(--vscode-font-weight);
        }

        #root {
            display: flex;
            flex-direction: column;
            min-height: 100vh;
        }

        .empty {
            padding: 8px 10px;
            color: var(--vscode-descriptionForeground);
            font-style: normal;
        }

        .tree-row {
            display: flex;
            align-items: center;
            gap: 6px;
            min-height: 22px;
            padding: 0 8px;
            box-sizing: border-box;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .tree-row:hover {
            background: var(--vscode-list-hoverBackground);
        }

        .node {
            border: 0;
            border-bottom: 1px solid transparent;
        }

        .node-header {
            cursor: pointer;
            user-select: none;
        }

        .group-header {
            cursor: pointer;
            user-select: none;
        }

        .disclosure {
            width: 12px;
            flex: 0 0 12px;
            color: var(--vscode-descriptionForeground);
            display: inline-flex;
            align-items: center;
            justify-content: center;
        }

        .disclosure svg {
            display: block;
            width: 12px;
            height: 12px;
            fill: currentColor;
        }

        .disclosure svg.mirrored {
            transform: scaleX(-1);
            transform-origin: center;
        }

        .spacer {
            width: 10px;
            flex: 0 0 10px;
        }

        .node-icon {
            width: 16px;
            height: 16px;
            flex: 0 0 16px;
            opacity: 0.95;
        }

        .label {
            overflow: hidden;
            text-overflow: ellipsis;
            flex: 0 1 auto;
            user-select: none;
        }

        .description {
            flex: 0 1 auto;
            color: var(--vscode-descriptionForeground);
            overflow: hidden;
            text-overflow: ellipsis;
            user-select: none;
        }

        .node-children {
            display: block;
        }

        .node.collapsed > .node-children {
            display: none;
        }

        .group {
            display: block;
        }

        .group.collapsed > .group-ports {
            display: none;
        }

        .group-header {
            padding-left: 24px;
        }

        .port-row {
            padding-left: 42px;
        }

        .port-icon {
            width: ${portIconSizePx}px;
            flex: 0 0 ${portIconSizePx}px;
            color: var(--vscode-terminal-ansiWhite);
            text-align: center;
            opacity: 0.95;
            display: inline-flex;
            align-items: center;
            justify-content: center;
        }

        .port-icon svg {
            display: block;
            width: ${portIconSizePx}px;
            height: ${portIconSizePx}px;
            fill: currentColor;
        }

        .port-icon.mirrored svg {
            transform: scaleX(-1);
            transform-origin: center;
        }

        .port-row[data-direction="input"][data-connectivity="connected"] .port-icon,
        .port-row[data-direction="input"][data-connectivity="mixed"] .port-icon {
            color: var(--vscode-terminal-ansiYellow);
        }

        .port-row[data-direction="output"][data-connectivity="connected"] .port-icon,
        .port-row[data-direction="output"][data-connectivity="mixed"] .port-icon {
            color: var(--vscode-terminal-ansiBlue);
        }

        .port-connectivity {
            margin-left: auto;
            color: var(--vscode-descriptionForeground);
            text-transform: none;
        }
    </style>
</head>
<body>
    <div id="root"></div>
    <script nonce="${nonce}">
        const root = document.getElementById("root");
        const state = {
            nodes: [],
            expanded: new Map(),
        };
        const icons = {
            merged: ${JSON.stringify(String(mergedIconUri))},
            single: ${JSON.stringify(String(singleIconUri))},
            arrowRight: ${JSON.stringify(arrowRightSvg)},
            chevronRight: ${JSON.stringify(chevronRightSvg)},
            chevronDown: ${JSON.stringify(chevronDownSvg)},
        };

        function expandedValue(key, defaultValue) {
            return state.expanded.has(key) ? state.expanded.get(key) : defaultValue;
        }

        function toggleExpanded(key, defaultValue) {
            state.expanded.set(key, !expandedValue(key, defaultValue));
            render();
        }

        function row(label, description) {
            const element = document.createElement("div");
            element.className = "tree-row";

            const labelEl = document.createElement("div");
            labelEl.className = "label";
            labelEl.textContent = label;
            element.appendChild(labelEl);

            if (description) {
                const descriptionEl = document.createElement("div");
                descriptionEl.className = "description";
                descriptionEl.textContent = description;
                element.appendChild(descriptionEl);
            }

            return element;
        }

        function disclosure(isExpanded) {
            const el = document.createElement("div");
            el.className = "disclosure";
            el.innerHTML = isExpanded
                ? icons.chevronDown
                : icons.chevronRight.replace("<svg ", '<svg class="mirrored" ');
            return el;
        }

        function spacer() {
            const el = document.createElement("div");
            el.className = "spacer";
            return el;
        }

        function renderPort(parent, nodeId, group, port, index) {
            const portKey = \`node:\${nodeId}/group:\${group.label}/port:\${index}\`;
            const portRow = row(port.name, port.connectivity);
            portRow.classList.add("port-row");
            portRow.dataset.direction = group.direction;
            portRow.dataset.connectivity = port.connectivity;
            portRow.prepend(spacer());

            const icon = document.createElement("div");
            icon.className = group.direction === "input" ? "port-icon" : "port-icon mirrored";
            icon.innerHTML = icons.arrowRight;
            portRow.insertBefore(icon, portRow.children[1]);
            portRow.title = \`\${port.name} • \${port.connectivity}\`;
            parent.appendChild(portRow);
            return portKey;
        }

        function renderGroup(parent, nodeId, group) {
            const groupKey = \`node:\${nodeId}/group:\${group.label}\`;
            const isExpanded = expandedValue(groupKey, true);

            const groupEl = document.createElement("div");
            groupEl.className = isExpanded ? "group" : "group collapsed";

            const header = row(group.label, String(group.count));
            header.classList.add("group-header");
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(groupKey, true));
            groupEl.appendChild(header);

            const ports = document.createElement("div");
            ports.className = "group-ports";
            for (let i = 0; i < group.ports.length; ++i) {
                renderPort(ports, nodeId, group, group.ports[i], i);
            }
            groupEl.appendChild(ports);
            parent.appendChild(groupEl);
        }

        function renderNode(parent, node) {
            const nodeKey = \`node:\${node.id}\`;
            const isExpanded = expandedValue(nodeKey, true);

            const nodeEl = document.createElement("div");
            nodeEl.className = isExpanded ? "node" : "node collapsed";

            const header = row(node.kind, node.description);
            header.classList.add("node-header");
            header.title = node.tooltip || node.kind;
            header.prepend(disclosure(isExpanded));

            const icon = document.createElement("img");
            icon.className = "node-icon";
            icon.src = node.icon === "merged" ? icons.merged : icons.single;
            icon.alt = "";
            header.insertBefore(icon, header.children[1]);
            header.addEventListener("click", () => toggleExpanded(nodeKey, true));
            nodeEl.appendChild(header);

            const children = document.createElement("div");
            children.className = "node-children";
            for (const group of node.groups) {
                renderGroup(children, node.id, group);
            }
            nodeEl.appendChild(children);
            parent.appendChild(nodeEl);
        }

        function render() {
            root.textContent = "";
            if (!Array.isArray(state.nodes) || state.nodes.length === 0) {
                const empty = document.createElement("div");
                empty.className = "empty";
                empty.textContent = "[no nodes]";
                empty.title = "No visible logical nodes at the current selection";
                root.appendChild(empty);
                return;
            }

            for (const node of state.nodes) {
                renderNode(root, node);
            }
        }

        window.addEventListener("message", (event) => {
            const message = event.data || {};
            if (message.type === "setState") {
                state.nodes = Array.isArray(message.nodes) ? message.nodes : [];
                render();
            }
        });

        render();
    </script>
</body>
</html>`;
    }
}

class NodeSpanHighlighter {
    constructor() {
        this.spans = [];
        this.activeRegions = [];
        this.decorationType = vscode.window.createTextEditorDecorationType({
            backgroundColor: "rgba(118, 173, 255, 0.14)",
            borderColor: "rgba(118, 173, 255, 0.60)",
            borderStyle: "solid",
            borderWidth: "1px",
            rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
            overviewRulerColor: "rgba(118, 173, 255, 0.75)",
            overviewRulerLane: vscode.OverviewRulerLane.Right,
        });
        this.regionDecorationType = vscode.window.createTextEditorDecorationType({
            backgroundColor: "rgba(160, 190, 255, 0.045)",
            borderColor: "rgba(160, 190, 255, 0.10)",
            borderStyle: "solid",
            borderWidth: "1px",
            rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
        });
    }

    dispose() {
        this.clear();
        this.decorationType.dispose();
        this.regionDecorationType.dispose();
    }

    clear() {
        this.spans = [];
        this.activeRegions = [];
        for (const editor of vscode.window.visibleTextEditors) {
            editor.setDecorations(this.decorationType, []);
            editor.setDecorations(this.regionDecorationType, []);
        }
    }

    clearPrimary() {
        this.spans = [];
        this.refresh();
    }

    setSpans(spans) {
        this.spans = Array.isArray(spans) ? spans : [];
        this.refresh();
    }

    setActiveRegions(spans) {
        this.activeRegions = Array.isArray(spans) ? spans : [];
        this.refresh();
    }

    refresh() {
        for (const editor of vscode.window.visibleTextEditors) {
            this.applyToEditor(editor);
        }
    }

    applyToEditor(editor) {
        if (editor.document.uri.scheme !== "file") {
            editor.setDecorations(this.decorationType, []);
            editor.setDecorations(this.regionDecorationType, []);
            return;
        }

        const filePath = editor.document.uri.fsPath;
        const decorations = this.spans
            .filter((span) => span.filePath === filePath)
            .map((span) => new vscode.Range(
                Math.max(span.start.line - 1, 0),
                Math.max(span.start.column - 1, 0),
                Math.max(span.end.line - 1, 0),
                Math.max(span.end.column - 1, 0)
            ));
        const primaryKeys = new Set(decorations.map((range) => `${range.start.line}:${range.start.character}:${range.end.line}:${range.end.character}`));
        const regionDecorations = this.activeRegions
            .filter((span) => span.filePath === filePath)
            .map((span) => new vscode.Range(
                Math.max(span.start.line - 1, 0),
                Math.max(span.start.column - 1, 0),
                Math.max(span.end.line - 1, 0),
                Math.max(span.end.column - 1, 0)
            ))
            .filter((range) => !primaryKeys.has(`${range.start.line}:${range.start.character}:${range.end.line}:${range.end.character}`));
        editor.setDecorations(this.decorationType, decorations);
        editor.setDecorations(this.regionDecorationType, regionDecorations);
    }
}

class WorkspaceSession {
    constructor(workspaceFolder, outputChannel, provider, highlighter) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.highlighter = highlighter;
        this.process = null;
        this.client = null;
        this.executionEpoch = 0;
        this.lastQueryError = "";
        this.refreshInFlight = null;
        this.lastQuery = null;
        this.startInFlight = null;
        this.lastTerminalStatusMessage = "";
    }

    resolveServerBinary() {
        const envDir = process.env.INTRAVENOUS_DIR;
        if (envDir && envDir.length > 0) {
            if (!path.isAbsolute(envDir)) {
                throw new Error(`INTRAVENOUS_DIR must be absolute: ${envDir}`);
            }
            return {
                source: "INTRAVENOUS_DIR",
                path: path.join(envDir, "intravenous"),
            };
        }

        const configured = vscode.workspace.getConfiguration("intravenous", this.workspaceFolder.uri).get("intravenousDir");
        if (!configured) {
            throw new Error("Intravenous executable directory is not configured. Set INTRAVENOUS_DIR or intravenous.intravenousDir.");
        }

        if (!path.isAbsolute(configured)) {
            throw new Error(`intravenous.intravenousDir must be absolute: ${configured}`);
        }

        return {
            source: "intravenous.intravenousDir",
            path: path.join(configured, "intravenous"),
        };
    }

    workspaceRoot() {
        return this.workspaceFolder.uri.fsPath;
    }

    projectMarkerPath() {
        return path.join(this.workspaceRoot(), ".intravenous");
    }

    isIntravenousProject() {
        return fs.existsSync(this.projectMarkerPath());
    }

    serverBinaryPath() {
        const resolved = this.resolveServerBinary();
        let stat;
        try {
            stat = fs.statSync(resolved.path);
        } catch (_) {
            throw new Error(`resolved server binary from ${resolved.source} does not exist: ${resolved.path}`);
        }

        if (!stat.isFile()) {
            throw new Error(`resolved server binary from ${resolved.source} is not a file: ${resolved.path}`);
        }

        fs.accessSync(resolved.path, fs.constants.X_OK);
        this.outputChannel.appendLine(`resolved Intravenous server from ${resolved.source}: ${resolved.path}`);
        return resolved.path;
    }

    socketPath() {
        const hash = crypto.createHash("sha1").update(this.workspaceRoot()).digest("hex").slice(0, 16);
        const dir = path.join(os.tmpdir(), "intravenous");
        fs.mkdirSync(dir, { recursive: true });
        return path.join(dir, `workspace-${hash}.sock`);
    }

    async start() {
        if (this.client) {
            return true;
        }
        if (this.startInFlight) {
            return await this.startInFlight;
        }
        this.startInFlight = this.startImpl().finally(() => {
            this.startInFlight = null;
        });
        return await this.startInFlight;
    }

    async startImpl() {
        if (!this.isIntravenousProject()) {
            return false;
        }
        this.lastTerminalStatusMessage = "";

        const socketPath = this.socketPath();
        const binary = this.serverBinaryPath();
        const serverDir = path.dirname(binary);
        const args = [
            "--server",
            "--workspace-root",
            this.workspaceRoot(),
            "--socket-path",
            socketPath,
        ];
        const childEnv = {
            ...process.env,
            INTRAVENOUS_DIR: process.env.INTRAVENOUS_DIR || serverDir,
        };

        if (await this.tryConnectExisting(socketPath, 2000)) {
            await this.initializeConnectedClient();
            this.outputChannel.appendLine(`connected to existing Intravenous server: ${socketPath}`);
            return true;
        }

        try {
            fs.unlinkSync(socketPath);
        } catch (_) {
        }

        this.outputChannel.appendLine(`starting Intravenous server: ${binary}`);
        this.process = childProcess.spawn(binary, args, {
            cwd: this.workspaceRoot(),
            env: childEnv,
            stdio: ["ignore", "ignore", "ignore"],
        });

        this.process.on("error", (error) => {
            this.outputChannel.appendLine(`Intravenous server spawn failed: ${error.message}`);
        });
        this.process.on("exit", (code, signal) => {
            this.outputChannel.appendLine(`Intravenous server exited: code=${code} signal=${signal}`);
        });

        await this.waitForSocket(socketPath, 10000);

        if (!(await this.tryConnectExisting(socketPath, 10000))) {
            throw new Error(`Intravenous server socket appeared but did not accept connections: ${socketPath}`);
        }
        await this.initializeConnectedClient();
        return true;
    }

    async initializeConnectedClient() {
        try {
            const result = await this.client.request("server.initialize", {
                workspaceRoot: this.workspaceRoot(),
            });
            this.executionEpoch = result.executionEpoch;
            this.lastTerminalStatusMessage = "";
            return result;
        } catch (error) {
            if (error.message === "Intravenous server connection closed" && this.lastTerminalStatusMessage) {
                throw new Error(this.lastTerminalStatusMessage);
            }
            throw error;
        }
    }

    async waitForSocket(socketPath, timeoutMs) {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            if (fs.existsSync(socketPath)) {
                return;
            }
            await new Promise((resolve) => setTimeout(resolve, 50));
        }
        throw new Error(`Intravenous server socket did not appear: ${socketPath}`);
    }

    async tryConnectExisting(socketPath, timeoutMs = 1000) {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            if (!fs.existsSync(socketPath)) {
                await new Promise((resolve) => setTimeout(resolve, 50));
                continue;
            }

            const client = new JsonRpcSocketClient(socketPath, (method, params) => this.handleNotification(method, params));
            try {
                await client.connect(Math.min(1000, Math.max(deadline - Date.now(), 100)));
                this.client = client;
                return true;
            } catch (_) {
                client.dispose();
                await new Promise((resolve) => setTimeout(resolve, 50));
            }
        }
        return false;
    }

    updatePrimaryHighlight(nodes) {
        if (!Array.isArray(nodes) || nodes.length === 0) {
            this.highlighter.clearPrimary();
            return;
        }

        const spans = [];
        const seen = new Set();
        for (const node of nodes) {
            if (!node || !Array.isArray(node.sourceSpans)) {
                continue;
            }
            for (const span of node.sourceSpans) {
                if (!span || !span.filePath || !span.start || !span.end) {
                    continue;
                }
                const key = [
                    span.filePath,
                    span.start.line,
                    span.start.column,
                    span.end.line,
                    span.end.column,
                ].join(":");
                if (seen.has(key)) {
                    continue;
                }
                seen.add(key);
                spans.push(span);
            }
        }

        if (spans.length > 0) {
            this.highlighter.setSpans(spans);
            return;
        }

        this.highlighter.clearPrimary();
    }

    async updateFromEditor(editor) {
        if (!this.client || !editor) {
            return [];
        }
        if (editor.document.uri.scheme !== "file") {
            return [];
        }

        const ranges = editor.selections.map((selection) => ({
            start: { line: selection.start.line + 1, column: selection.start.character + 1 },
            end: { line: selection.end.line + 1, column: selection.end.character + 1 },
        }));
        if (ranges.length === 0) {
            return [];
        }

        this.lastQuery = {
            filePath: editor.document.uri.fsPath,
            ranges,
        };

        const result = await this.client.request("graph.queryBySpans", {
            filePath: editor.document.uri.fsPath,
            ranges,
            match: ranges.length > 1 ? "union" : "intersection",
        });
        const activeRegionsResult = await this.client.request("graph.queryActiveRegions", {
            filePath: editor.document.uri.fsPath,
        });
        this.executionEpoch = result.executionEpoch || this.executionEpoch;
        this.lastQueryError = "";
        const nodes = this.sortNodesByRelevance(result.nodes || [], this.lastQuery);
        this.provider.setNodes(nodes);
        this.highlighter.setActiveRegions(activeRegionsResult.sourceSpans || []);
        return nodes;
    }

    async ensureReady() {
        if (!this.isIntravenousProject()) {
            this.outputChannel.appendLine(`workspace is not an Intravenous project: missing ${this.projectMarkerPath()}`);
            return false;
        }
        if (!this.client) {
            await this.start();
        }
        return true;
    }

    positionKey(position) {
        return (position.line * 1000000) + position.column;
    }

    sortNodesByRelevance(nodes, query) {
        if (!query) {
            return nodes;
        }

        const scoreNode = (node) => {
            let best = null;
            for (const span of node.sourceSpans || []) {
                if (span.filePath !== query.filePath) {
                    continue;
                }
                const spanStart = this.positionKey(span.start);
                const spanEnd = this.positionKey(span.end);
                const spanLength = Math.max(spanEnd - spanStart, 0);

                for (let rangeIndex = 0; rangeIndex < query.ranges.length; ++rangeIndex) {
                    const range = query.ranges[rangeIndex];
                    const rangeStart = this.positionKey(range.start);
                    const rangeEnd = this.positionKey(range.end);
                    const boundaryDistance = Math.abs(spanStart - rangeStart) + Math.abs(spanEnd - rangeEnd);
                    const score = [rangeIndex, boundaryDistance, spanLength, spanStart];
                    if (
                        !best ||
                        score[0] < best[0] ||
                        (score[0] === best[0] && (
                            score[1] < best[1] ||
                            (score[1] === best[1] && (
                                score[2] < best[2] ||
                                (score[2] === best[2] && score[3] < best[3])
                            ))
                        ))
                    ) {
                        best = score;
                    }
                }
            }

            return best || [
                Number.MAX_SAFE_INTEGER,
                Number.MAX_SAFE_INTEGER,
                Number.MAX_SAFE_INTEGER,
                Number.MAX_SAFE_INTEGER,
            ];
        };

        return [...nodes].sort((left, right) => {
            const a = scoreNode(left);
            const b = scoreNode(right);
            if (a[0] !== b[0]) {
                return a[0] - b[0];
            }
            if (a[1] !== b[1]) {
                return a[1] - b[1];
            }
            if (a[2] !== b[2]) {
                return a[2] - b[2];
            }
            if (a[3] !== b[3]) {
                return a[3] - b[3];
            }
            return String(left.id).localeCompare(String(right.id));
        });
    }

    logServerState(prefix, params) {
        const parts = [prefix];
        if (params.moduleRoot) {
            parts.push(params.moduleRoot);
        }
        if (params.executionEpoch) {
            parts.push(`epoch=${params.executionEpoch}`);
        }
        if (params.message) {
            parts.push(params.message);
        } else if (params.code) {
            parts.push(params.code);
        }
        this.outputChannel.appendLine(parts.join(": "));
    }

    handleNotification(method, params) {
        if (method === "server.message") {
            if (params.message) {
                const lines = String(params.message).split(/\r?\n/);
                for (const line of lines) {
                    if (line.length > 0) {
                        this.outputChannel.appendLine(line);
                    }
                }
            }
            return;
        }

        if (method !== "server.status") {
            return;
        }

        if (params.code === "startupFailed") {
            this.lastTerminalStatusMessage = params.message || "Intravenous startup failed";
            this.logServerState("Intravenous startup failed", params);
            return;
        }

        if (params.code === "rebuildStarted") {
            this.logServerState("Intravenous rebuild started", params);
            return;
        }

        if (params.code === "rebuildFinished") {
            if (typeof params.executionEpoch === "number") {
                this.executionEpoch = params.executionEpoch;
            }
            if (Array.isArray(params.deletedNodeIds)) {
                this.provider.pruneDeletedNodeState(params.deletedNodeIds);
            }
            this.logServerState("Intravenous rebuild finished", params);
            if (!this.refreshInFlight && vscode.window.activeTextEditor) {
                this.refreshInFlight = this.updateFromEditor(vscode.window.activeTextEditor)
                    .then((nodes) => {
                        this.updatePrimaryHighlight(nodes);
                    })
                    .catch((error) => {
                        const message = `Intravenous query failed: ${error.message}`;
                        if (message !== this.lastQueryError) {
                            this.outputChannel.appendLine(message);
                            this.lastQueryError = message;
                        }
                    })
                    .finally(() => {
                        this.refreshInFlight = null;
                    });
            }
            return;
        }

        if (params.code === "rebuildFailed") {
            this.logServerState("Intravenous rebuild failed", params);
            return;
        }

        this.logServerState("Intravenous status", params);
    }

    async shutdown() {
        if (this.client) {
            try {
                await this.client.request("server.shutdown", {});
            } catch (_) {
            }
        }
        if (this.client) {
            this.client.dispose();
            this.client = null;
        }
        if (this.process) {
            this.process.kill();
            this.process = null;
        }
    }
}

async function activate(context) {
    const outputChannel = vscode.window.createOutputChannel("Intravenous");
    const provider = new LiveGraphViewProvider(context.extensionUri);
    const highlighter = new NodeSpanHighlighter();
    context.subscriptions.push(outputChannel);
    context.subscriptions.push(highlighter);
    context.subscriptions.push(vscode.window.registerWebviewViewProvider("intravenous.liveGraph", provider, {
        webviewOptions: {
            retainContextWhenHidden: true,
        },
    }));

    const workspaceFolder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!workspaceFolder) {
        return;
    }

    const session = new WorkspaceSession(workspaceFolder, outputChannel, provider, highlighter);
    context.subscriptions.push({ dispose: () => void session.shutdown() });
    context.subscriptions.push(vscode.window.onDidChangeVisibleTextEditors(() => {
        highlighter.refresh();
    }));
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        if (editor) {
            highlighter.applyToEditor(editor);
        }
    }));

    if (!session.isIntravenousProject()) {
        outputChannel.appendLine(`workspace is not an Intravenous project: missing ${session.projectMarkerPath()}`);
        return;
    }

    context.subscriptions.push(vscode.languages.registerDocumentHighlightProvider(
        { scheme: "file", language: "cpp" },
        {
            provideDocumentHighlights: async (document, position) => {
                const owningWorkspace = vscode.workspace.getWorkspaceFolder(document.uri);
                if (!owningWorkspace || owningWorkspace.uri.fsPath !== session.workspaceRoot()) {
                    return undefined;
                }

                try {
                    if (!(await session.ensureReady())) {
                        return undefined;
                    }
                } catch (_) {
                    return undefined;
                }

                try {
                    const result = await session.client.request("graph.queryBySpans", {
                        filePath: document.uri.fsPath,
                        ranges: [{
                            start: { line: position.line + 1, column: position.character + 1 },
                            end: { line: position.line + 1, column: position.character + 1 },
                        }],
                        match: "intersection",
                    });

                    if (Array.isArray(result.nodes) && result.nodes.length > 0) {
                        return [];
                    }
                } catch (_) {
                }

                return undefined;
            },
        }
    ));

    try {
        await session.start();
        if (vscode.window.activeTextEditor) {
            const nodes = await session.updateFromEditor(vscode.window.activeTextEditor);
            session.updatePrimaryHighlight(nodes);
        }
    } catch (error) {
        outputChannel.appendLine(`Intravenous startup failed: ${error.message}`);
        throw error;
    }

    context.subscriptions.push(vscode.window.onDidChangeTextEditorSelection(async (event) => {
        if (event.textEditor.document.uri.scheme !== "file") {
            return;
        }
        try {
            const nodes = await session.updateFromEditor(event.textEditor);
            session.updatePrimaryHighlight(nodes);
        } catch (error) {
            const message = `Intravenous query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
        }
    }));
}

function deactivate() {}

module.exports = {
    activate,
    deactivate,
};

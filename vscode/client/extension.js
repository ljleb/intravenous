const vscode = require("vscode");
const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const net = require("net");
const os = require("os");
const path = require("path");
const mergedNodeIconPath = path.join(__dirname, "media", "merged_node.svg");

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

class LiveGraphItem extends vscode.TreeItem {
    constructor(label, collapsibleState, description = "") {
        super(label, collapsibleState);
        this.description = description;
        this.children = [];
        this.loadChildren = null;
        this.childrenLoaded = true;
        this.treeKey = "";
    }
}

class LiveGraphProvider {
    constructor() {
        this.items = [];
        this.nodes = [];
        this.logicalNodeResolver = null;
        this.expandedState = new Map();
        this.onDidChangeTreeDataEmitter = new vscode.EventEmitter();
        this.onDidChangeTreeData = this.onDidChangeTreeDataEmitter.event;
    }

    setLogicalNodeResolver(resolver) {
        this.logicalNodeResolver = resolver;
    }

    setNodes(nodes) {
        this.nodes = nodes;
        this.items = nodes.length > 0
            ? nodes.map((node) => this.makeNodeItem(node))
            : [this.makeEmptyItem()];
        this.onDidChangeTreeDataEmitter.fire();
    }

    makeEmptyItem() {
        const item = new LiveGraphItem("[no nodes]", vscode.TreeItemCollapsibleState.None, "");
        item.treeKey = "empty";
        item.contextValue = "intravenousEmpty";
        item.tooltip = "No visible logical nodes at the current selection";
        return item;
    }

    setExpanded(element, expanded) {
        if (!element || !element.treeKey) {
            return;
        }
        this.expandedState.set(element.treeKey, expanded);
    }

    pruneDeletedNodeState(deletedNodeIds) {
        if (!Array.isArray(deletedNodeIds) || deletedNodeIds.length === 0) {
            return;
        }
        const deleted = new Set(deletedNodeIds);
        for (const key of [...this.expandedState.keys()]) {
            let shouldDelete = false;
            for (const nodeId of deleted) {
                if (key.includes(`node:${nodeId}`) || key.includes(`member:${nodeId}`)) {
                    shouldDelete = true;
                    break;
                }
            }
            if (shouldDelete) {
                this.expandedState.delete(key);
            }
        }
    }

    collapsibleStateFor(treeKey, defaultExpanded) {
        const expanded = this.expandedState.has(treeKey)
            ? this.expandedState.get(treeKey)
            : defaultExpanded;
        return expanded
            ? vscode.TreeItemCollapsibleState.Expanded
            : vscode.TreeItemCollapsibleState.Collapsed;
    }

    getTreeItem(element) {
        return element;
    }

    async getChildren(element) {
        if (!element) {
            return this.items;
        }
        if (element.loadChildren && !element.childrenLoaded) {
            element.children = await element.loadChildren();
            element.childrenLoaded = true;
        }
        return element.children || [];
    }

    makeNodeItem(node) {
        const treeKey = `node:${node.id}`;
        const item = new LiveGraphItem(
            node.kind || node.id,
            this.collapsibleStateFor(treeKey, true),
            node.memberCount > 1 ? `${node.memberCount} nodes` : (node.id || "")
        );
        item.treeKey = treeKey;
        item.contextValue = "intravenousNode";
        item.node = node;
        item.tooltip = `${node.kind || "node"}${Array.isArray(node.sourceSpans) ? ` • ${node.sourceSpans.length} source span${node.sourceSpans.length === 1 ? "" : "s"}` : ""}${node.memberCount > 1 ? ` • ${node.memberCount} members` : ""}`;
        item.iconPath = node.memberCount > 1
            ? mergedNodeIconPath
            : new vscode.ThemeIcon("symbol-misc");

        const children = [];
        children.push(this.makePortGroup(treeKey, "sample inputs", node.sampleInputs || [], "input", "sample"));
        children.push(this.makePortGroup(treeKey, "sample outputs", node.sampleOutputs || [], "output", "sample"));
        children.push(this.makePortGroup(treeKey, "event inputs", node.eventInputs || [], "input", "event"));
        children.push(this.makePortGroup(treeKey, "event outputs", node.eventOutputs || [], "output", "event"));
        if (Array.isArray(node.memberNodeIds) && node.memberNodeIds.length > 1) {
            children.push(this.makeMembersGroup(treeKey, node.memberNodeIds));
        }
        item.children = children;
        return item;
    }

    makePortGroup(parentTreeKey, label, ports, direction, portKind) {
        const treeKey = `${parentTreeKey}/group:${label}`;
        const item = new LiveGraphItem(label, this.collapsibleStateFor(treeKey, true), `${ports.length}`);
        item.treeKey = treeKey;
        item.children = ports.map((port, index) => {
            const connectivity = port.connectivity || "disconnected";
            const description = [port.type || "sample", connectivity].join(" · ");
            const child = new LiveGraphItem(port.name || `${label} ${index}`, vscode.TreeItemCollapsibleState.None, description);
            child.iconPath = this.portIcon(direction, portKind, connectivity);
            return child;
        });
        return item;
    }

    makeMemberNodeItem(parentTreeKey, member) {
        const treeKey = `${parentTreeKey}/member:${member.id}`;
        const item = new LiveGraphItem(
            member.id || member.kind || "member",
            this.collapsibleStateFor(treeKey, false),
            member.kind || ""
        );
        item.treeKey = treeKey;
        item.tooltip = `${member.kind || "member"}${Array.isArray(member.sourceSpans) ? ` • ${member.sourceSpans.length} source span${member.sourceSpans.length === 1 ? "" : "s"}` : ""}`;
        item.iconPath = new vscode.ThemeIcon("symbol-misc");
        item.children = [
            this.makePortGroup(treeKey, "sample inputs", member.sampleInputs || [], "input", "sample"),
            this.makePortGroup(treeKey, "sample outputs", member.sampleOutputs || [], "output", "sample"),
            this.makePortGroup(treeKey, "event inputs", member.eventInputs || [], "input", "event"),
            this.makePortGroup(treeKey, "event outputs", member.eventOutputs || [], "output", "event"),
        ];
        return item;
    }

    hydrateMemberItem(item, member) {
        item.label = member.id || member.kind || "member";
        item.description = member.kind || "";
        item.tooltip = `${member.kind || "member"}${Array.isArray(member.sourceSpans) ? ` • ${member.sourceSpans.length} source span${member.sourceSpans.length === 1 ? "" : "s"}` : ""}`;
        item.iconPath = new vscode.ThemeIcon("symbol-misc");
        item.children = [
            this.makePortGroup(item.treeKey, "sample inputs", member.sampleInputs || [], "input", "sample"),
            this.makePortGroup(item.treeKey, "sample outputs", member.sampleOutputs || [], "output", "sample"),
            this.makePortGroup(item.treeKey, "event inputs", member.eventInputs || [], "input", "event"),
            this.makePortGroup(item.treeKey, "event outputs", member.eventOutputs || [], "output", "event"),
        ];
        item.childrenLoaded = true;
        item.memberLoaded = true;
        item.loadChildren = null;
    }

    makeMemberPlaceholder(parentTreeKey, memberNodeId, allMemberNodeIds, memberItems, index) {
        const treeKey = `${parentTreeKey}/member:${memberNodeId}`;
        const item = new LiveGraphItem(
            memberNodeId,
            this.collapsibleStateFor(treeKey, false),
            ""
        );
        item.treeKey = treeKey;
        item.iconPath = new vscode.ThemeIcon("symbol-misc");
        item.children = [];
        item.childrenLoaded = false;
        item.memberLoaded = false;
        item.loadChildren = async () => {
            if (item.memberLoaded) {
                return item.children;
            }
            if (!this.logicalNodeResolver) {
                item.childrenLoaded = true;
                item.memberLoaded = true;
                return item.children;
            }

            const start = Math.max(0, index - 5);
            const end = Math.min(allMemberNodeIds.length, index + 6);
            const nearbyIds = allMemberNodeIds.slice(start, end);
            const resolvedMembers = await this.logicalNodeResolver(nearbyIds);
            const membersById = new Map((resolvedMembers || []).map((member) => [member.id, member]));

            for (let i = start; i < end; ++i) {
                const sibling = memberItems[i];
                const member = membersById.get(allMemberNodeIds[i]);
                if (!sibling || !member) {
                    continue;
                }
                this.hydrateMemberItem(sibling, member);
                this.onDidChangeTreeDataEmitter.fire(sibling);
            }

            return item.children;
        };
        return item;
    }

    makeMembersGroup(parentTreeKey, memberNodeIds) {
        const treeKey = `${parentTreeKey}/group:members`;
        const item = new LiveGraphItem("members", this.collapsibleStateFor(treeKey, true), `${memberNodeIds.length}`);
        item.treeKey = treeKey;
        const memberItems = new Array(memberNodeIds.length);
        for (let index = 0; index < memberNodeIds.length; ++index) {
            memberItems[index] = this.makeMemberPlaceholder(treeKey, memberNodeIds[index], memberNodeIds, memberItems, index);
        }
        item.children = memberItems;
        return item;
    }

    portIcon(direction, portKind, connectivity) {
        const iconName = direction === "input" ? "arrow-right" : "arrow-left";
        const color = new vscode.ThemeColor(
            connectivity === "disconnected"
                ? "terminal.ansiWhite"
                : (direction === "input" ? "terminal.ansiYellow" : "terminal.ansiBlue")
        );
        return new vscode.ThemeIcon(iconName, color);
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
        for (const editor of vscode.window.visibleTextEditors) {
            editor.setDecorations(this.decorationType, []);
        }
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
        if (!this.isIntravenousProject()) {
            return false;
        }

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

        if (await this.tryConnectExisting(socketPath)) {
            const result = await this.client.request("server.initialize", {
                workspaceRoot: this.workspaceRoot(),
            });
            this.executionEpoch = result.executionEpoch;
            this.outputChannel.appendLine(`connected to existing Intravenous server: ${socketPath}`);
            return;
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

        this.client = new JsonRpcSocketClient(socketPath, (method, params) => this.handleNotification(method, params));
        await this.client.connect();
        const result = await this.client.request("server.initialize", {
            workspaceRoot: this.workspaceRoot(),
        });
        this.executionEpoch = result.executionEpoch;
        return true;
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

    async tryConnectExisting(socketPath) {
        if (!fs.existsSync(socketPath)) {
            return false;
        }

        const client = new JsonRpcSocketClient(socketPath, (method, params) => this.handleNotification(method, params));
        try {
            await client.connect(1000);
            this.client = client;
            return true;
        } catch (_) {
            client.dispose();
            return false;
        }
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

    async fetchLogicalNodes(nodeIds) {
        if (!this.client || !Array.isArray(nodeIds) || nodeIds.length === 0) {
            return [];
        }
        const nodes = await this.client.request("graph.getLogicalNodes", {
            executionEpoch: this.executionEpoch,
            nodeIds,
        });
        return Array.isArray(nodes) ? nodes : [];
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

                for (const range of query.ranges) {
                    const rangeStart = this.positionKey(range.start);
                    const rangeEnd = this.positionKey(range.end);
                    const boundaryDistance = Math.abs(spanStart - rangeStart) + Math.abs(spanEnd - rangeEnd);
                    const score = [boundaryDistance, spanLength, spanStart];
                    if (!best || score[0] < best[0] || (score[0] === best[0] && (score[1] < best[1] || (score[1] === best[1] && score[2] < best[2])))) {
                        best = score;
                    }
                }
            }

            return best || [Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER];
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
            return String(left.id).localeCompare(String(right.id));
        });
    }

    logServerEvent(prefix, params) {
        const parts = [prefix];
        if (params.moduleRoot) {
            parts.push(params.moduleRoot);
        }
        if (params.executionEpoch) {
            parts.push(`epoch=${params.executionEpoch}`);
        }
        if (params.message) {
            parts.push(params.message);
        }
        this.outputChannel.appendLine(parts.join(": "));
    }

    handleNotification(method, params) {
        if (method === "server.log") {
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

        if (method === "server.buildStarted") {
            this.logServerEvent("Intravenous rebuild started", params);
            return;
        }

        if (method === "server.buildFinished") {
            if (typeof params.executionEpoch === "number") {
                this.executionEpoch = params.executionEpoch;
            }
            if (Array.isArray(params.deletedNodeIds)) {
                this.provider.pruneDeletedNodeState(params.deletedNodeIds);
            }
            this.logServerEvent("Intravenous rebuild finished", params);
            if (!this.refreshInFlight && vscode.window.activeTextEditor) {
                this.refreshInFlight = this.updateFromEditor(vscode.window.activeTextEditor)
                    .then((nodes) => {
                        const primary = Array.isArray(nodes) && nodes.length > 0 ? nodes[0] : null;
                        if (primary && Array.isArray(primary.sourceSpans)) {
                            this.highlighter.setSpans(primary.sourceSpans);
                        } else {
                            this.highlighter.clearPrimary();
                        }
                    })
                    .catch((error) => {
                        const message = `Intravenous query failed: ${error.message}`;
                        if (message !== this.lastQueryError) {
                            this.outputChannel.appendLine(message);
                            this.lastQueryError = message;
                        }
                        this.highlighter.clearPrimary();
                    })
                    .finally(() => {
                        this.refreshInFlight = null;
                    });
            }
            return;
        }

        if (method === "server.buildFailed") {
            this.logServerEvent("Intravenous rebuild failed", params);
        }
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
    const provider = new LiveGraphProvider();
    const highlighter = new NodeSpanHighlighter();
    context.subscriptions.push(outputChannel);
    context.subscriptions.push(highlighter);
    const treeView = vscode.window.createTreeView("intravenous.liveGraph", { treeDataProvider: provider });
    context.subscriptions.push(treeView);
    context.subscriptions.push(treeView.onDidExpandElement((event) => {
        provider.setExpanded(event.element, true);
    }));
    context.subscriptions.push(treeView.onDidCollapseElement((event) => {
        provider.setExpanded(event.element, false);
    }));

    const workspaceFolder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!workspaceFolder) {
        return;
    }

    const session = new WorkspaceSession(workspaceFolder, outputChannel, provider, highlighter);
    provider.setLogicalNodeResolver((nodeIds) => session.fetchLogicalNodes(nodeIds));
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
            const primary = Array.isArray(nodes) && nodes.length > 0 ? nodes[0] : null;
            if (primary && Array.isArray(primary.sourceSpans)) {
                highlighter.setSpans(primary.sourceSpans);
            } else {
                highlighter.clearPrimary();
            }
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
            const primary = Array.isArray(nodes) && nodes.length > 0 ? nodes[0] : null;
            if (primary && Array.isArray(primary.sourceSpans)) {
                highlighter.setSpans(primary.sourceSpans);
            } else {
                highlighter.clearPrimary();
            }
        } catch (error) {
            const message = `Intravenous query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
            highlighter.clearPrimary();
        }
    }));
}

function deactivate() {}

module.exports = {
    activate,
    deactivate,
};

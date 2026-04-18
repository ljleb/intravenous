const vscode = require("vscode");
const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const net = require("net");
const os = require("os");
const path = require("path");

class JsonRpcSocketClient {
    constructor(socketPath) {
        this.socketPath = socketPath;
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
    }
}

class LiveGraphProvider {
    constructor() {
        this.items = [];
        this.onDidChangeTreeDataEmitter = new vscode.EventEmitter();
        this.onDidChangeTreeData = this.onDidChangeTreeDataEmitter.event;
    }

    setNodes(nodes) {
        this.items = nodes.map((node) => this.makeNodeItem(node));
        this.onDidChangeTreeDataEmitter.fire();
    }

    getTreeItem(element) {
        return element;
    }

    getChildren(element) {
        if (!element) {
            return this.items;
        }
        return element.children || [];
    }

    makeNodeItem(node) {
        const item = new LiveGraphItem(
            node.id,
            vscode.TreeItemCollapsibleState.Expanded,
            node.kind || ""
        );
        item.contextValue = "intravenousNode";

        const children = [];
        if (Array.isArray(node.sourceSpans)) {
            for (const span of node.sourceSpans) {
                children.push(new LiveGraphItem(
                    `${path.basename(span.filePath)}:${span.start.line}:${span.start.column}-${span.end.line}:${span.end.column}`,
                    vscode.TreeItemCollapsibleState.None,
                    "source"
                ));
            }
        }

        children.push(this.makePortGroup("sample inputs", node.sampleInputs || []));
        children.push(this.makePortGroup("sample outputs", node.sampleOutputs || []));
        children.push(this.makePortGroup("event inputs", node.eventInputs || []));
        children.push(this.makePortGroup("event outputs", node.eventOutputs || []));
        item.children = children;
        return item;
    }

    makePortGroup(label, ports) {
        const item = new LiveGraphItem(label, vscode.TreeItemCollapsibleState.Collapsed, `${ports.length}`);
        item.children = ports.map((port, index) => {
            const description = [port.type || "sample", port.connected ? "connected" : "disconnected"].join(" · ");
            return new LiveGraphItem(port.name || `${label} ${index}`, vscode.TreeItemCollapsibleState.None, description);
        });
        return item;
    }
}

class WorkspaceSession {
    constructor(workspaceFolder, outputChannel, provider) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.process = null;
        this.client = null;
        this.executionEpoch = 0;
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
            stdio: ["ignore", "pipe", "pipe"],
        });

        this.process.stdout.on("data", (chunk) => this.outputChannel.append(chunk.toString()));
        this.process.stderr.on("data", (chunk) => this.outputChannel.append(chunk.toString()));
        this.process.on("error", (error) => {
            this.outputChannel.appendLine(`Intravenous server spawn failed: ${error.message}`);
        });
        this.process.on("exit", (code, signal) => {
            this.outputChannel.appendLine(`Intravenous server exited: code=${code} signal=${signal}`);
        });

        await this.waitForSocket(socketPath, 10000);

        this.client = new JsonRpcSocketClient(socketPath);
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

        const client = new JsonRpcSocketClient(socketPath);
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
            return;
        }
        if (editor.document.uri.scheme !== "file") {
            return;
        }

        const ranges = editor.selections.map((selection) => ({
            start: { line: selection.start.line + 1, column: selection.start.character + 1 },
            end: { line: selection.end.line + 1, column: selection.end.character + 1 },
        }));
        if (ranges.length === 0) {
            return;
        }

        const result = await this.client.request("graph.queryBySpans", {
            filePath: editor.document.uri.fsPath,
            ranges,
        });
        this.executionEpoch = result.executionEpoch;
        this.provider.setNodes(result.nodes || []);
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
    context.subscriptions.push(outputChannel);
    context.subscriptions.push(vscode.window.registerTreeDataProvider("intravenous.liveGraph", provider));

    const workspaceFolder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!workspaceFolder) {
        return;
    }

    const session = new WorkspaceSession(workspaceFolder, outputChannel, provider);
    context.subscriptions.push({ dispose: () => void session.shutdown() });

    if (!session.isIntravenousProject()) {
        outputChannel.appendLine(`workspace is not an Intravenous project: missing ${session.projectMarkerPath()}`);
        return;
    }

    try {
        await session.start();
        if (vscode.window.activeTextEditor) {
            await session.updateFromEditor(vscode.window.activeTextEditor);
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
            await session.updateFromEditor(event.textEditor);
        } catch (error) {
            outputChannel.appendLine(`Intravenous query failed: ${error.message}`);
        }
    }));
}

function deactivate() {}

module.exports = {
    activate,
    deactivate,
};

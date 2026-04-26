const vscode = require("vscode");
const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const os = require("os");
const path = require("path");

const { JsonRpcSocketClient } = require("./rpcClient");

class WorkspaceSession {
    constructor(workspaceFolder, outputChannel, provider, laneProvider, highlighter) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.laneProvider = laneProvider;
        this.highlighter = highlighter;
        this.process = null;
        this.client = null;
        this.executionEpoch = 0;
        this.lastQueryError = "";
        this.refreshInFlight = null;
        this.lastQuery = null;
        this.startInFlight = null;
        this.lastTerminalStatusMessage = "";
        this.laneViewId = `lanes-${crypto.randomBytes(8).toString("hex")}`;
        this.laneViewOpen = false;
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

    async setSampleInputValue(nodeId, inputOrdinal, value, memberOrdinal = null) {
        if (!(await this.ensureReady())) {
            return;
        }
        const params = {
            executionEpoch: this.executionEpoch,
            nodeId,
            inputOrdinal,
            value,
        };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        await this.client.request("graph.setSampleInputValue", params);
        this.provider.updateSampleInputValue(nodeId, inputOrdinal, value, memberOrdinal);
    }

    async clearSampleInputValueOverride(nodeId, memberOrdinal, inputOrdinal) {
        if (!(await this.ensureReady())) {
            return;
        }
        await this.client.request("graph.clearSampleInputValueOverride", {
            executionEpoch: this.executionEpoch,
            nodeId,
            memberOrdinal,
            inputOrdinal,
        });
        this.provider.clearSampleInputValueOverride(nodeId, memberOrdinal, inputOrdinal);
    }

    laneViewRequestParams() {
        const viewport = this.laneProvider.viewportState();
        return {
            viewId: this.laneViewId,
            executionEpoch: this.executionEpoch,
            filter: { kind: "graphInputs" },
            startIndex: viewport.startIndex,
            visibleLaneCount: viewport.visibleLaneCount,
        };
    }

    async openLaneView() {
        if (!(await this.ensureReady()) || !this.executionEpoch) {
            this.laneProvider.clear();
            return;
        }
        const result = await this.client.request("timeline.openLaneView", this.laneViewRequestParams());
        this.laneViewOpen = true;
        this.laneProvider.setLanes(result);
        await this.updateLaneViewVisibleLanes();
    }

    async updateLaneViewVisibleLanes() {
        if (!this.laneViewOpen || !(await this.ensureReady()) || !this.executionEpoch) {
            return;
        }
        const result = await this.client.request("timeline.updateLaneView", this.laneViewRequestParams());
        this.laneProvider.setLanes(result);
    }

    async closeLaneView() {
        if (!this.laneViewOpen || !this.client) {
            return;
        }
        this.laneViewOpen = false;
        try {
            await this.client.request("timeline.closeLaneView", {
                viewId: this.laneViewId,
            });
        } catch (_) {
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

        if (method === "timeline.laneViewUpdated") {
            if (params.viewId === this.laneViewId) {
                this.laneProvider.setLanes(params);
                this.updateLaneViewVisibleLanes().catch((error) => {
                    this.outputChannel.appendLine(`Intravenous lane view update failed: ${error.message}`);
                });
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

module.exports = { WorkspaceSession };

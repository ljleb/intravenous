import * as vscode from "vscode";
import * as childProcess from "child_process";
import * as crypto from "crypto";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

import { collectPrimarySourceSpans, QueryShape, sortNodesByRelevance } from "./graphQueryModel";
import { LogicalNode, SourceRange } from "./graphModel";
import { LiveGraphControlMessage } from "./liveGraphProtocol";
import { JsonRpcSocketClient } from "./rpcClient";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceNotificationRouter } from "./workspaceNotifications";
import { WorkspaceRpc } from "./workspaceRpc";

type LiveGraphProviderLike = {
    setNodes(nodes: LogicalNode[]): void;
    updateSampleInputValue(nodeId: string, inputOrdinal: number, value: unknown, memberOrdinal?: number | null): void;
    clearSampleInputValueOverride(nodeId: string, memberOrdinal: number, inputOrdinal: number): void;
    pruneDeletedNodeState(nodeIds: string[]): void;
};

type LaneProviderLike = {
    viewportState(): { startIndex: number; visibleLaneCount: number };
    clear(): void;
    setLanes(result: Record<string, unknown>): void;
};

type ServerStatusNotification = {
    code?: string;
    message?: string;
    moduleRoot?: string;
    deletedNodeIds?: string[];
};

type ServerMessageNotification = {
    message?: string;
};

type LaneViewUpdatedNotification = Record<string, unknown> & {
    viewId?: string;
};

export class WorkspaceSession {
    private readonly workspaceFolder: vscode.WorkspaceFolder;
    private readonly outputChannel: vscode.OutputChannel;
    readonly provider: LiveGraphProviderLike;
    private readonly laneProvider: LaneProviderLike;
    private readonly highlighter: NodeSpanHighlighter;
    private readonly notifications = new WorkspaceNotificationRouter();
    private process: childProcess.ChildProcess | null = null;
    private client: JsonRpcSocketClient | null = null;
    private rpc: WorkspaceRpc | null = null;
    lastQueryError = "";
    private refreshInFlight: Promise<void> | null = null;
    private lastQuery: QueryShape | null = null;
    private startInFlight: Promise<boolean> | null = null;
    private lastTerminalStatusMessage = "";
    private readonly laneViewId = `lanes-${crypto.randomBytes(8).toString("hex")}`;
    private laneViewOpen = false;

    constructor(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        laneProvider: LaneProviderLike,
        highlighter: NodeSpanHighlighter,
    ) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.laneProvider = laneProvider;
        this.highlighter = highlighter;
        this.registerNotificationHandlers();
    }

    private registerNotificationHandlers(): void {
        this.notifications.subscribe<ServerMessageNotification>("server.message", (params) => {
            if (!params.message) {
                return;
            }
            const lines = String(params.message).split(/\r?\n/);
            for (const line of lines) {
                if (line.length > 0) {
                    this.outputChannel.appendLine(line);
                }
            }
        });

        this.notifications.subscribe<LaneViewUpdatedNotification>("timeline.laneViewUpdated", (params) => {
            if (params.viewId === this.laneViewId) {
                this.laneProvider.setLanes(params);
            }
        });

        this.notifications.subscribe<ServerStatusNotification>("server.status", async (params) => {
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
                if (Array.isArray(params.deletedNodeIds)) {
                    this.provider.pruneDeletedNodeState(params.deletedNodeIds);
                }
                this.logServerState("Intravenous rebuild finished", params);
                if (!this.refreshInFlight && vscode.window.activeTextEditor) {
                    this.refreshInFlight = this.updateFromEditor(vscode.window.activeTextEditor)
                        .then((nodes) => {
                            this.updatePrimaryHighlight(nodes);
                        })
                        .catch((error: Error) => {
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
        });
    }

    private resolveServerBinary(): { source: string; path: string } {
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

        const configured = vscode.workspace
            .getConfiguration("intravenous", this.workspaceFolder.uri)
            .get<string>("intravenousDir");
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

    workspaceRoot(): string {
        return this.workspaceFolder.uri.fsPath;
    }

    projectMarkerPath(): string {
        return path.join(this.workspaceRoot(), ".intravenous");
    }

    isIntravenousProject(): boolean {
        return fs.existsSync(this.projectMarkerPath());
    }

    private serverBinaryPath(): string {
        const resolved = this.resolveServerBinary();
        let stat: fs.Stats;
        try {
            stat = fs.statSync(resolved.path);
        } catch {
            throw new Error(`resolved server binary from ${resolved.source} does not exist: ${resolved.path}`);
        }

        if (!stat.isFile()) {
            throw new Error(`resolved server binary from ${resolved.source} is not a file: ${resolved.path}`);
        }

        fs.accessSync(resolved.path, fs.constants.X_OK);
        this.outputChannel.appendLine(`resolved Intravenous server from ${resolved.source}: ${resolved.path}`);
        return resolved.path;
    }

    private socketPath(): string {
        const hash = crypto.createHash("sha1").update(this.workspaceRoot()).digest("hex").slice(0, 16);
        const dir = path.join(os.tmpdir(), "intravenous");
        fs.mkdirSync(dir, { recursive: true });
        return path.join(dir, `workspace-${hash}.sock`);
    }

    async start(): Promise<boolean> {
        if (this.rpc) {
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

    private async startImpl(): Promise<boolean> {
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
        } catch {
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

    private async initializeConnectedClient(): Promise<unknown> {
        if (!this.rpc) {
            throw new Error("Intravenous RPC session is not connected");
        }

        try {
            const result = await this.rpc.initialize(this.workspaceRoot());
            this.lastTerminalStatusMessage = "";
            return result;
        } catch (error: any) {
            if (error.message === "Intravenous server connection closed" && this.lastTerminalStatusMessage) {
                throw new Error(this.lastTerminalStatusMessage);
            }
            throw error;
        }
    }

    async dispatchLiveGraphControl(message: LiveGraphControlMessage): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }

        switch (message.type) {
        case "setSampleInputValue":
            await this.rpc.setSampleInputValue(
                message.nodeId,
                message.inputOrdinal,
                message.value,
                message.memberOrdinal ?? null,
            );
            this.provider.updateSampleInputValue(
                message.nodeId,
                message.inputOrdinal,
                message.value,
                message.memberOrdinal ?? null,
            );
            return;

        case "setSampleInputState":
            await this.rpc.setSampleInputState(
                message.nodeId,
                message.inputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            if (message.state === "default" && message.memberOrdinal != null) {
                this.provider.clearSampleInputValueOverride(
                    message.nodeId,
                    message.memberOrdinal,
                    message.inputOrdinal,
                );
            }
            return;

        case "setEventInputState":
            await this.rpc.setEventInputState(
                message.nodeId,
                message.inputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;

        case "setSampleOutputState":
            await this.rpc.setSampleOutputState(
                message.nodeId,
                message.outputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;

        case "setEventOutputState":
            await this.rpc.setEventOutputState(
                message.nodeId,
                message.outputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;
        }
    }

    private laneViewRequestParams(): { viewId: string; filter: { kind: string }; startIndex: number; visibleLaneCount: number } {
        const viewport = this.laneProvider.viewportState();
        return {
            viewId: this.laneViewId,
            filter: { kind: "graphInputs" },
            startIndex: viewport.startIndex,
            visibleLaneCount: viewport.visibleLaneCount,
        };
    }

    async openLaneView(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            this.laneProvider.clear();
            return;
        }
        const result = await this.rpc.openLaneView(this.laneViewRequestParams());
        this.laneViewOpen = true;
        this.laneProvider.setLanes(result);
        await this.updateLaneViewVisibleLanes();
    }

    async updateLaneViewVisibleLanes(): Promise<void> {
        if (!this.laneViewOpen || !(await this.ensureReady()) || !this.rpc) {
            return;
        }
        const result = await this.rpc.updateLaneView(this.laneViewRequestParams());
        this.laneProvider.setLanes(result);
    }

    async closeLaneView(): Promise<void> {
        if (!this.laneViewOpen || !this.rpc) {
            return;
        }
        this.laneViewOpen = false;
        try {
            await this.rpc.closeLaneView(this.laneViewId);
        } catch {
        }
    }

    private async waitForSocket(socketPath: string, timeoutMs: number): Promise<void> {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            if (fs.existsSync(socketPath)) {
                return;
            }
            await new Promise((resolve) => setTimeout(resolve, 50));
        }
        throw new Error(`Intravenous server socket did not appear: ${socketPath}`);
    }

    private async tryConnectExisting(socketPath: string, timeoutMs = 1000): Promise<boolean> {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            if (!fs.existsSync(socketPath)) {
                await new Promise((resolve) => setTimeout(resolve, 50));
                continue;
            }

            const client = new JsonRpcSocketClient(socketPath, (method, params) => {
                void this.notifications.dispatch(method, params);
            });
            try {
                await client.connect(Math.min(1000, Math.max(deadline - Date.now(), 100)));
                this.client = client;
                this.rpc = new WorkspaceRpc(client);
                return true;
            } catch {
                client.dispose();
                await new Promise((resolve) => setTimeout(resolve, 50));
            }
        }
        return false;
    }

    updatePrimaryHighlight(nodes: LogicalNode[]): void {
        if (!Array.isArray(nodes) || nodes.length === 0) {
            this.highlighter.clearPrimary();
            return;
        }

        const spans = collectPrimarySourceSpans(nodes);
        if (spans.length > 0) {
            this.highlighter.setSpans(spans);
            return;
        }

        this.highlighter.clearPrimary();
    }

    private selectionRanges(editor: vscode.TextEditor): SourceRange[] {
        return editor.selections.map((selection) => ({
            start: { line: selection.start.line + 1, column: selection.start.character + 1 },
            end: { line: selection.end.line + 1, column: selection.end.character + 1 },
        }));
    }

    async updateFromEditor(editor: vscode.TextEditor | undefined): Promise<LogicalNode[]> {
        if (!this.rpc || !editor) {
            return [];
        }
        if (editor.document.uri.scheme !== "file") {
            return [];
        }

        const ranges = this.selectionRanges(editor);
        if (ranges.length === 0) {
            return [];
        }

        this.lastQuery = {
            filePath: editor.document.uri.fsPath,
            ranges,
        };

        const result = await this.rpc.queryNodesBySpans(
            editor.document.uri.fsPath,
            ranges,
            ranges.length > 1 ? "union" : "intersection",
        );
        const activeRegionsResult = await this.rpc.queryActiveRegions(editor.document.uri.fsPath);
        this.lastQueryError = "";
        const nodes = sortNodesByRelevance(result.nodes || [], this.lastQuery);
        this.provider.setNodes(nodes);
        this.highlighter.setActiveRegions(activeRegionsResult.sourceSpans || []);
        return nodes;
    }

    async hasNodesAtPosition(document: vscode.TextDocument, position: vscode.Position): Promise<boolean> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return false;
        }

        const result = await this.rpc.queryNodesBySpans(document.uri.fsPath, [{
            start: { line: position.line + 1, column: position.character + 1 },
            end: { line: position.line + 1, column: position.character + 1 },
        }], "intersection");

        return Array.isArray(result.nodes) && result.nodes.length > 0;
    }

    async ensureReady(): Promise<boolean> {
        if (!this.isIntravenousProject()) {
            this.outputChannel.appendLine(`workspace is not an Intravenous project: missing ${this.projectMarkerPath()}`);
            return false;
        }
        if (!this.rpc) {
            await this.start();
        }
        return true;
    }

    private logServerState(prefix: string, params: ServerStatusNotification): void {
        const parts = [prefix];
        if (params.moduleRoot) {
            parts.push(params.moduleRoot);
        }
        if (params.message) {
            parts.push(params.message);
        } else if (params.code) {
            parts.push(params.code);
        }
        this.outputChannel.appendLine(parts.join(": "));
    }

    async shutdown(): Promise<void> {
        if (this.rpc) {
            try {
                await this.rpc.shutdown();
            } catch {
            }
        }
        if (this.client) {
            this.client.dispose();
            this.client = null;
        }
        this.rpc = null;
        if (this.process) {
            this.process.kill();
            this.process = null;
        }
    }
}

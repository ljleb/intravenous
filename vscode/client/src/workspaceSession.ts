import * as vscode from "vscode";
import * as childProcess from "child_process";
import * as crypto from "crypto";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { Duplex } from "stream";

import { collectPrimarySourceSpans, QueryShape, sortNodesByRelevance } from "./graphQueryModel";
import { LogicalNode, LogicalNodeMember, LogicalPort, SourcePosition, SourceRange, SourceSpan } from "./graphModel";
import { LiveGraphControlMessage } from "./liveGraphProtocol";
import { JsonRpcSocketClient } from "./rpcClient";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { autoDetectedServerDirectoriesForWorkspaceRoot } from "./serverBinaryPaths";
import { WorkspaceNotificationRouter } from "./workspaceNotifications";
import { WorkspaceRpc } from "./workspaceRpc";

type LiveGraphProviderLike = {
    setInstances(instances: IvModuleInstanceInfo[]): void;
    setNodes(nodes: LogicalNode[]): void;
    upsertNodes(nodes: LogicalNode[], replaceInstanceIds?: string[]): void;
    setSelectedInstanceId(instanceId: string | null): void;
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

type ServerReadyNotification = Record<string, never>;

type LaneViewUpdatedNotification = Record<string, unknown> & {
    viewId?: string;
};

type IvModuleInstanceInfo = {
    instanceId?: string;
    definitionId?: string;
    moduleId?: string;
    moduleRoot?: string;
    realized?: boolean;
};

type IvModuleInstancesUpdatedNotification = {
    instances?: IvModuleInstanceInfo[];
};

type GraphNodesUpdatedNotification = {
    nodes?: unknown[];
    replaceInstanceIds?: string[];
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
    private lastQuery: QueryShape | null = null;
    private startInFlight: Promise<boolean> | null = null;
    private lastTerminalStatusMessage = "";
    private readonly laneViewId = `lanes-${crypto.randomBytes(8).toString("hex")}`;
    private laneViewOpen = false;
    private serverStdoutLines: string[] = [];
    private serverStderrLines: string[] = [];
    private readonly maxCapturedServerLogLines = 20;
    private serverReadyReceived = false;
    private serverReadyWaiters: Array<{
        resolve: () => void;
        reject: (error: Error) => void;
        timeout: NodeJS.Timeout;
    }> = [];
    private ivModuleInstances: IvModuleInstanceInfo[] = [];
    private selectedInstanceId: string | null = null;

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

        this.notifications.subscribe<ServerReadyNotification>("server.ready", () => {
            this.serverReadyReceived = true;
            this.outputChannel.appendLine("Intravenous server ready");
            this.resolveServerReadyWaiters();
        });

        this.notifications.subscribe<LaneViewUpdatedNotification>("timeline.laneViewUpdated", (params) => {
            if (params.viewId === this.laneViewId) {
                this.laneProvider.setLanes(params);
            }
        });

        this.notifications.subscribe<IvModuleInstancesUpdatedNotification>("ivModuleInstances.updated", (params) => {
            this.ivModuleInstances = Array.isArray(params.instances) ? params.instances : [];
            this.selectedInstanceId = this.resolveSelectedInstanceId(
                this.ivModuleInstances,
                this.selectedInstanceId,
            );
            this.provider.setInstances(this.ivModuleInstances);
            this.provider.setSelectedInstanceId(this.selectedInstanceId);
        });

        this.notifications.subscribe<GraphNodesUpdatedNotification>("graph.nodesUpdated", async (params) => {
            const nodes = this.parseLogicalNodes(params.nodes);
            const replaceInstanceIds = Array.isArray(params.replaceInstanceIds)
                ? params.replaceInstanceIds.filter((value): value is string => typeof value === "string")
                : [];
            const editor = vscode.window.activeTextEditor;
            const hasActiveQuery =
                !!this.lastQuery &&
                !!editor &&
                editor.document.uri.scheme === "file" &&
                editor.document.uri.fsPath === this.lastQuery.filePath;
            if (hasActiveQuery) {
                const refreshed = await this.updateFromEditor(editor);
                this.updatePrimaryHighlight(refreshed);
                return;
            }
            this.provider.upsertNodes(nodes, replaceInstanceIds);
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
                this.logServerState("Intravenous rebuild finished", params);
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

        for (const candidate of this.autoDetectedServerDirectories()) {
            if (this.binaryExists(candidate)) {
                return {
                    source: candidate.source,
                    path: path.join(candidate.directory, "intravenous"),
                };
            }
        }

        const configured = vscode.workspace
            .getConfiguration("intravenous", this.workspaceFolder.uri)
            .get<string>("intravenousDir");
        if (!configured) {
            throw new Error(
                "Intravenous executable directory is not configured. " +
                "Set INTRAVENOUS_DIR or intravenous.intravenousDir, " +
                "or build the repo so the client can auto-detect build/src/intravenous.");
        }

        if (!path.isAbsolute(configured)) {
            throw new Error(`intravenous.intravenousDir must be absolute: ${configured}`);
        }

        return {
            source: "intravenous.intravenousDir",
            path: path.join(configured, "intravenous"),
        };
    }

    private autoDetectedServerDirectories(): Array<{ source: string; directory: string }> {
        return autoDetectedServerDirectoriesForWorkspaceRoot(this.workspaceRoot());
    }

    private parseIvModuleInstances(payload: unknown): IvModuleInstanceInfo[] {
        if (!Array.isArray(payload)) {
            return [];
        }
        return payload.filter((candidate): candidate is IvModuleInstanceInfo =>
            !!candidate && typeof candidate === "object");
    }

    private parseSourcePosition(payload: unknown): SourcePosition | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        const candidate = payload as Record<string, unknown>;
        if (typeof candidate.line !== "number" || typeof candidate.column !== "number") {
            return null;
        }
        return {
            line: candidate.line,
            column: candidate.column,
        };
    }

    private parseSourceSpan(payload: unknown): SourceSpan | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        const candidate = payload as Record<string, unknown>;
        if (typeof candidate.filePath !== "string") {
            return null;
        }

        const directStart = this.parseSourcePosition(candidate.start);
        const directEnd = this.parseSourcePosition(candidate.end);
        if (directStart && directEnd) {
            return {
                filePath: candidate.filePath,
                start: directStart,
                end: directEnd,
            };
        }

        const range = candidate.range;
        if (!range || typeof range !== "object") {
            return null;
        }
        const rangeCandidate = range as Record<string, unknown>;
        const rangeStart = this.parseSourcePosition(rangeCandidate.start);
        const rangeEnd = this.parseSourcePosition(rangeCandidate.end);
        if (!rangeStart || !rangeEnd) {
            return null;
        }
        return {
            filePath: candidate.filePath,
            start: rangeStart,
            end: rangeEnd,
        };
    }

    private parseLogicalPort(payload: unknown): LogicalPort | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        return payload as LogicalPort;
    }

    private parseLogicalNodeMember(payload: unknown): LogicalNodeMember | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        const candidate = payload as Record<string, unknown>;
        return {
            ordinal: typeof candidate.ordinal === "number" ? candidate.ordinal : undefined,
            backingNodeId: typeof candidate.backingNodeId === "string" ? candidate.backingNodeId : undefined,
            kind: typeof candidate.kind === "string" ? candidate.kind : undefined,
            typeIdentity: typeof candidate.typeIdentity === "string" ? candidate.typeIdentity : undefined,
            sampleInputs: Array.isArray(candidate.sampleInputs)
                ? candidate.sampleInputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                : undefined,
            sampleOutputs: Array.isArray(candidate.sampleOutputs)
                ? candidate.sampleOutputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                : undefined,
            eventInputs: Array.isArray(candidate.eventInputs)
                ? candidate.eventInputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                : undefined,
            eventOutputs: Array.isArray(candidate.eventOutputs)
                ? candidate.eventOutputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                : undefined,
        };
    }

    private parseLogicalNodes(payload: unknown): LogicalNode[] {
        if (!Array.isArray(payload)) {
            return [];
        }
        return payload.flatMap((entry) => {
            if (!entry || typeof entry !== "object") {
                return [];
            }
            const candidate = entry as Record<string, unknown>;
            const sourceSpans = Array.isArray(candidate.sourceSpans)
                ? candidate.sourceSpans.map((span) => this.parseSourceSpan(span)).filter((span): span is SourceSpan => span !== null)
                : undefined;
            const members = Array.isArray(candidate.members)
                ? candidate.members.map((member) => this.parseLogicalNodeMember(member)).filter((member): member is LogicalNodeMember => member !== null)
                : undefined;
            return [{
                id: typeof candidate.id === "string" ? candidate.id : undefined,
                instanceId: typeof candidate.instanceId === "string" ? candidate.instanceId : undefined,
                kind: typeof candidate.kind === "string" ? candidate.kind : undefined,
                sourceIdentity: typeof candidate.sourceIdentity === "string" ? candidate.sourceIdentity : undefined,
                typeIdentity: typeof candidate.typeIdentity === "string" ? candidate.typeIdentity : undefined,
                sourceSpans,
                sampleInputs: Array.isArray(candidate.sampleInputs)
                    ? candidate.sampleInputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                    : undefined,
                sampleOutputs: Array.isArray(candidate.sampleOutputs)
                    ? candidate.sampleOutputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                    : undefined,
                eventInputs: Array.isArray(candidate.eventInputs)
                    ? candidate.eventInputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                    : undefined,
                eventOutputs: Array.isArray(candidate.eventOutputs)
                    ? candidate.eventOutputs.map((port) => this.parseLogicalPort(port)).filter((port): port is LogicalPort => port !== null)
                    : undefined,
                memberCount: typeof candidate.memberCount === "number" ? candidate.memberCount : undefined,
                members,
            }];
        });
    }

    private parseSourceSpans(payload: unknown): SourceSpan[] {
        if (!Array.isArray(payload)) {
            return [];
        }
        return payload.map((span) => this.parseSourceSpan(span)).filter((span): span is SourceSpan => span !== null);
    }

    private binaryExists(candidate: { directory: string }): boolean {
        try {
            const stat = fs.statSync(path.join(candidate.directory, "intravenous"));
            return stat.isFile();
        } catch {
            return false;
        }
    }

    workspaceRoot(): string {
        return this.workspaceFolder.uri.fsPath;
    }

    projectMarkerPath(): string {
        return path.join(this.workspaceRoot(), "project.intravenous");
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
        this.resetServerReadyState();

        const binary = this.serverBinaryPath();
        const serverDir = path.dirname(binary);
        const args = [
            "--server",
            "--workspace-root",
            this.workspaceRoot(),
            "--rpc-fd",
            "3",
        ];
        const childEnv = {
            ...process.env,
            INTRAVENOUS_DIR: process.env.INTRAVENOUS_DIR || serverDir,
        };

        this.resetCapturedServerLogs();
        this.outputChannel.appendLine(`starting Intravenous server: ${binary}`);
        this.outputChannel.appendLine(`Intravenous server cwd: ${this.workspaceRoot()}`);
        this.process = childProcess.spawn(binary, args, {
            cwd: this.workspaceRoot(),
            env: childEnv,
            stdio: ["ignore", "pipe", "pipe", "pipe"],
        });
        this.attachServerOutputCapture(this.process);
        this.outputChannel.appendLine("Intravenous startup: server process spawned");

        const rpcStream = this.process.stdio[3];
        if (!this.isDuplexStream(rpcStream)) {
            throw new Error("Intravenous startup failed: child rpc fd 3 was not exposed as a duplex stream");
        }

        this.client = new JsonRpcSocketClient(rpcStream, (method, params) => {
            this.outputChannel.appendLine(`Intravenous startup notification: ${method}`);
            void this.notifications.dispatch(method, params);
        });
        this.outputChannel.appendLine("Intravenous startup: attaching client RPC socket");
        await this.client.connect();
        this.outputChannel.appendLine("Intravenous startup: client RPC socket attached");
        this.rpc = new WorkspaceRpc(this.client);

        this.process.on("error", (error) => {
            this.rejectServerReadyWaiters(new Error(`Intravenous server spawn failed: ${error.message}`));
            this.outputChannel.appendLine(`Intravenous server spawn failed: ${error.message}`);
        });
        this.process.on("exit", (code, signal) => {
            this.process = null;
            this.outputChannel.appendLine(`Intravenous server exited: code=${code} signal=${signal}`);
            this.logCapturedServerFailureContext();
            if (!this.serverReadyReceived) {
                this.rejectServerReadyWaiters(
                    new Error(`Intravenous server exited before reporting ready: code=${code} signal=${signal}`),
                );
            }
        });

        this.outputChannel.appendLine("Intravenous startup: waiting for server.ready");
        await this.waitForServerReady(10000);
        if (this.rpc) {
            const result = await this.rpc.getIvModuleInstances();
            this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
            this.selectedInstanceId = this.resolveSelectedInstanceId(
                this.ivModuleInstances,
                this.selectedInstanceId,
            );
            const realizedCount = this.ivModuleInstances.filter((instance) => instance.realized).length;
            this.outputChannel.appendLine(
                `Intravenous instances after ready: total=${this.ivModuleInstances.length} realized=${realizedCount} selected=${this.selectedInstanceId ?? "[none]"}`,
            );
            this.provider.setInstances(this.ivModuleInstances);
            this.provider.setSelectedInstanceId(this.selectedInstanceId);
        }
        return true;
    }

    private async waitForServerReady(timeoutMs: number): Promise<void> {
        if (this.serverReadyReceived) {
            return;
        }

        await new Promise<void>((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.serverReadyWaiters = this.serverReadyWaiters.filter((waiter) => waiter.timeout !== timeout);
                this.logCapturedServerFailureContext();
                reject(new Error("Intravenous server did not report ready before timeout"));
            }, timeoutMs);
            this.serverReadyWaiters.push({ resolve, reject, timeout });
        });
    }

    async dispatchLiveGraphControl(message: LiveGraphControlMessage): Promise<void> {
        switch (message.type) {
        case "selectInstance":
            this.selectedInstanceId = this.resolveSelectedInstanceId(
                this.ivModuleInstances,
                message.instanceId ?? null,
            );
            this.provider.setSelectedInstanceId(this.selectedInstanceId);
            await this.refreshActiveEditorSelection();
            return;

        case "setSampleInputValue":
            if (!(await this.ensureReady()) || !this.rpc) {
                return;
            }
            await this.rpc.setSampleInputValue(
                message.nodeId,
                message.inputOrdinal,
                message.value,
                message.memberOrdinal ?? null,
            );
            return;

        case "setSampleInputState":
            if (!(await this.ensureReady()) || !this.rpc) {
                return;
            }
            await this.rpc.setSampleInputState(
                message.nodeId,
                message.inputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;

        case "setEventInputState":
            if (!(await this.ensureReady()) || !this.rpc) {
                return;
            }
            await this.rpc.setEventInputState(
                message.nodeId,
                message.inputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;

        case "setSampleOutputState":
            if (!(await this.ensureReady()) || !this.rpc) {
                return;
            }
            await this.rpc.setSampleOutputState(
                message.nodeId,
                message.outputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;

        case "setEventOutputState":
            if (!(await this.ensureReady()) || !this.rpc) {
                return;
            }
            await this.rpc.setEventOutputState(
                message.nodeId,
                message.outputOrdinal,
                message.state,
                message.memberOrdinal ?? null,
            );
            return;
        }
    }

    private async refreshActiveEditorSelection(): Promise<void> {
        const editor = vscode.window.activeTextEditor;
        if (!editor) {
            return;
        }
        const nodes = await this.updateFromEditor(editor);
        this.updatePrimaryHighlight(nodes);
    }

    private resolveSelectedInstanceId(
        instances: IvModuleInstanceInfo[],
        requestedInstanceId: string | null,
    ): string | null {
        if (requestedInstanceId) {
            const selected = instances.find((instance) => instance.instanceId === requestedInstanceId);
            if (selected) {
                return requestedInstanceId;
            }
        }

        const realized = instances.find((instance) => instance.realized && typeof instance.instanceId === "string");
        if (realized?.instanceId) {
            return realized.instanceId;
        }

        const first = instances.find((instance) => typeof instance.instanceId === "string");
        return first?.instanceId || null;
    }

    async pausePlayback(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.pausePlayback();
    }

    async resumePlayback(startIndex = 0): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.resumePlayback(startIndex);
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
            this.selectedInstanceId,
        );
        const activeRegionsResult = await this.rpc.queryActiveRegions(editor.document.uri.fsPath);
        this.lastQueryError = "";
        const nodes = sortNodesByRelevance(this.parseLogicalNodes(result.nodes), this.lastQuery);
        const activeRegions = this.parseSourceSpans(activeRegionsResult.sourceSpans);
        if (nodes.length === 0 || activeRegions.length === 0) {
            this.outputChannel.appendLine(
                `Intravenous editor query: file=${editor.document.uri.fsPath} instance=${this.selectedInstanceId ?? "[none]"} nodes=${nodes.length} activeRegions=${activeRegions.length}`,
            );
        }
        this.provider.setNodes(nodes);
        this.highlighter.setActiveRegions(activeRegions);
        return nodes;
    }

    async hasNodesAtPosition(document: vscode.TextDocument, position: vscode.Position): Promise<boolean> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return false;
        }

        const result = await this.rpc.queryNodesBySpans(document.uri.fsPath, [{
            start: { line: position.line + 1, column: position.character + 1 },
            end: { line: position.line + 1, column: position.character + 1 },
        }], "intersection", this.selectedInstanceId);

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

    private resetCapturedServerLogs(): void {
        this.serverStdoutLines = [];
        this.serverStderrLines = [];
    }

    private attachServerOutputCapture(process: childProcess.ChildProcess): void {
        process.stdout?.setEncoding("utf8");
        process.stderr?.setEncoding("utf8");
        process.stdout?.on("data", (chunk: string | Buffer) => {
            this.captureServerOutput(this.serverStdoutLines, chunk.toString());
        });
        process.stderr?.on("data", (chunk: string | Buffer) => {
            this.captureServerOutput(this.serverStderrLines, chunk.toString());
        });
    }

    private captureServerOutput(target: string[], text: string): void {
        for (const line of text.split(/\r?\n/)) {
            const normalized = line.trim();
            if (normalized.length === 0) {
                continue;
            }
            target.push(normalized);
        }
        if (target.length > this.maxCapturedServerLogLines) {
            target.splice(0, target.length - this.maxCapturedServerLogLines);
        }
    }

    private logCapturedServerFailureContext(): void {
        if (this.serverStderrLines.length > 0) {
            this.outputChannel.appendLine("Intravenous server failure details:");
            for (const line of this.serverStderrLines) {
                this.outputChannel.appendLine(`  stderr: ${line}`);
            }
        } else if (this.serverStdoutLines.length > 0) {
            this.outputChannel.appendLine("Intravenous server recent stdout:");
            for (const line of this.serverStdoutLines) {
                this.outputChannel.appendLine(`  stdout: ${line}`);
            }
        }
        this.resetCapturedServerLogs();
    }

    private isDuplexStream(value: unknown): value is Duplex {
        return !!value
            && typeof value === "object"
            && "on" in value
            && typeof (value as { on?: unknown }).on === "function"
            && "write" in value
            && typeof (value as { write?: unknown }).write === "function";
    }

    private resetServerReadyState(): void {
        this.serverReadyReceived = false;
        this.rejectServerReadyWaiters(new Error("Intravenous server startup was restarted"));
    }

    private resolveServerReadyWaiters(): void {
        const waiters = this.serverReadyWaiters;
        this.serverReadyWaiters = [];
        for (const waiter of waiters) {
            clearTimeout(waiter.timeout);
            waiter.resolve();
        }
    }

    private rejectServerReadyWaiters(error: Error): void {
        const waiters = this.serverReadyWaiters;
        this.serverReadyWaiters = [];
        for (const waiter of waiters) {
            clearTimeout(waiter.timeout);
            waiter.reject(error);
        }
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

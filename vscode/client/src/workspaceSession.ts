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
import { ModuleInstanceInfo, ModuleSourceInfo, ModulesControlMessage } from "./modulesViewProvider";

type LiveGraphProviderLike = {
    setInstances(instances: IvModuleInstanceInfo[]): void;
    setNodes(nodes: LogicalNode[]): void;
    upsertNodes(nodes: LogicalNode[], replaceInstanceIds?: string[]): void;
    setSelectedInstanceId(instanceId: string | null): void;
    setModuleSource(moduleRoot: string | null): void;
};

type LaneProviderLike = {
    viewportState(): { startIndex: number; visibleLaneCount: number };
    clear(): void;
    setLanes(result: Record<string, unknown>): void;
    setLaneContent(result: Record<string, unknown>): void;
};

type ModulesProviderLike = {
    setState(sources: ModuleSourceInfo[], instances: ModuleInstanceInfo[], selectedInstanceId: string | null): void;
};

type ServerStatusNotification = {
    level?: string;
    code?: string;
    message?: string;
    moduleRoot?: string;
    deletedNodeIds?: string[];
};

type LaneViewContentNotification = Record<string, unknown> & {
    viewId?: string;
    lanes?: Array<Record<string, unknown>>;
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
    displayName?: string;
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
    private readonly modulesProvider: ModulesProviderLike;
    private readonly highlighter: NodeSpanHighlighter;
    private readonly rebuildStatusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    private readonly clangdDatabaseWatcher: vscode.FileSystemWatcher;
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
    private playbackPaused = true;
    private lastScrubbedSampleIndex = 0;
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
    private projectModuleInstances: IvModuleInstanceInfo[] = [];
    private ivModuleSources: ModuleSourceInfo[] = [];
    private selectedInstanceId: string | null = null;
    private activeSourceFilePath: string | null = null;
    private activeModuleRoot: string | null = null;
    private readonly selectedInstanceIdBySourceFile = new Map<string, string>();
    private clangdRestartTimer: NodeJS.Timeout | null = null;

    constructor(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        laneProvider: LaneProviderLike,
        modulesProvider: ModulesProviderLike,
        highlighter: NodeSpanHighlighter,
    ) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.laneProvider = laneProvider;
        this.modulesProvider = modulesProvider;
        this.highlighter = highlighter;
        this.clangdDatabaseWatcher = vscode.workspace.createFileSystemWatcher(
            new vscode.RelativePattern(this.workspaceFolder, "compile_commands.json"),
        );
        this.clangdDatabaseWatcher.onDidCreate(() => this.restartClangdForCompilationDatabase());
        this.clangdDatabaseWatcher.onDidChange(() => this.restartClangdForCompilationDatabase());
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

        this.notifications.subscribe<LaneViewContentNotification>("timeline.laneViewContentUpdated", (params) => {
            if (params.viewId === this.laneViewId) {
                this.laneProvider.setLaneContent(params);
            }
        });

        this.notifications.subscribe<IvModuleInstancesUpdatedNotification>("ivModuleInstances.updated", (params) => {
            this.projectModuleInstances = Array.isArray(params.instances) ? params.instances : [];
            if (this.activeSourceFilePath && this.rpc) {
                void this.rpc.getIvModuleInstances(this.activeSourceFilePath).then((result) => {
                    this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
                    this.refreshVisibleInstances();
                    this.refreshModulesPanelState();
                });
                return;
            }
            this.ivModuleInstances = this.projectModuleInstances;
            this.refreshVisibleInstances();
            this.refreshModulesPanelState();
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
                this.showRebuildStatus(params);
                return;
            }

            if (params.code === "rebuildFinished") {
                this.logServerState("Intravenous rebuild finished", params);
                this.rebuildStatusBar.hide();
                return;
            }

            if (params.code === "rebuildFailed") {
                this.logServerState("Intravenous rebuild failed", params);
                this.showRebuildFailure(params);
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

    private parseIvModuleSource(payload: unknown): ModuleSourceInfo | null {
        if (!payload || typeof payload !== "object") return null;
        const source = payload as Record<string, unknown>;
        if (typeof source.moduleId !== "string" || typeof source.moduleRoot !== "string") return null;
        return { moduleId: source.moduleId, moduleRoot: source.moduleRoot, projectLocal: source.projectLocal === true };
    }

    private parseIvModuleSources(payload: unknown): ModuleSourceInfo[] {
        if (!Array.isArray(payload)) return [];
        return payload.map((source) => this.parseIvModuleSource(source))
            .filter((source): source is ModuleSourceInfo => source !== null);
    }

    private modulePanelInstances(): ModuleInstanceInfo[] {
        return this.projectModuleInstances.flatMap((instance) => {
            if (typeof instance.instanceId !== "string" || typeof instance.definitionId !== "string" || typeof instance.moduleRoot !== "string") return [];
            return [{
                instanceId: instance.instanceId,
                definitionId: instance.definitionId,
                displayName: typeof instance.displayName === "string" && instance.displayName.length > 0
                    ? instance.displayName
                    : instance.definitionId,
                moduleId: instance.moduleId,
                moduleRoot: instance.moduleRoot,
                realized: instance.realized === true,
            }];
        });
    }

    private refreshModulesPanelState(): void {
        this.modulesProvider.setState(this.ivModuleSources, this.modulePanelInstances(), this.selectedInstanceId);
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
        return path.join(this.workspaceRoot(), "iv_project.jsonl");
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
            if (method !== "timeline.laneViewContentUpdated") {
                this.outputChannel.appendLine(`Intravenous startup notification: ${method}`);
            }
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
            this.rebuildStatusBar.hide();
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
            this.projectModuleInstances = this.ivModuleInstances;
            this.refreshVisibleInstances();
            const realizedCount = this.ivModuleInstances.filter((instance) => instance.realized).length;
            this.outputChannel.appendLine(
                `Intravenous instances after ready: total=${this.ivModuleInstances.length} realized=${realizedCount} selected=${this.selectedInstanceId ?? "[none]"}`,
            );
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
                this.visibleInstances(),
                message.instanceId ?? null,
            );
            this.rememberSelectedInstance();
            this.syncSelectedInstanceViews();
            await this.refreshActiveEditorSelection();
            return;

        case "createInstance":
            if (!(await this.ensureReady()) || !this.rpc || !this.activeModuleRoot) {
                return;
            }
            {
                const created = await this.rpc.createIvModuleInstance(
                    await this.moduleIdForRoot(this.activeModuleRoot),
                );
                this.selectedInstanceId = created.instanceId;
                this.rememberSelectedInstance();
                this.refreshVisibleInstances();
                await this.refreshActiveEditorSelection();
            }
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

    async refreshModulesPanel(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) return;
        const [sources, instances] = await Promise.all([
            this.rpc.getIvModuleSources(),
            this.rpc.getIvModuleInstances(),
        ]);
        this.ivModuleSources = this.parseIvModuleSources(sources.sources);
        this.projectModuleInstances = this.parseIvModuleInstances(instances.instances);
        this.refreshModulesPanelState();
    }

    async dispatchModulesControl(message: ModulesControlMessage): Promise<void> {
        // Instance selection is client-side presentation state.  In
        // particular, do not put rapid row clicks behind server readiness or
        // any RPC work.
        if (message.type === "select") {
            const selectedInstanceId = this.resolveSelectedInstanceId(
                this.visibleInstances(),
                message.instanceId,
            );
            if (selectedInstanceId === this.selectedInstanceId) {
                return;
            }
            this.selectedInstanceId = selectedInstanceId;
            this.rememberSelectedInstance();
            this.syncSelectedInstanceViews();
            return;
        }
        if (!(await this.ensureReady()) || !this.rpc) return;
        switch (message.type) {
        case "createSource": {
            const result = await this.rpc.createIvModuleSource(message.name);
            const source = this.parseIvModuleSource(result.source);
            await this.refreshModulesPanel();
            if (source) await this.revealModuleSource(source.moduleRoot);
            return;
        }
        case "instantiate":
        case "duplicate": {
            const created = await this.rpc.createIvModuleInstance(
                await this.moduleIdForRoot(message.moduleRoot),
            );
            this.selectedInstanceId = created.instanceId;
            await this.refreshModulesPanel();
            return;
        }
        case "open":
            this.selectedInstanceId = message.instanceId;
            this.syncSelectedInstanceViews();
            await this.revealModuleSource(message.moduleRoot);
            await this.refreshModulesPanel();
            return;
        case "rename":
            await this.rpc.updateIvModuleInstances([{
                instanceId: message.instanceId,
                displayName: message.displayName,
            }]);
            await this.refreshModulesPanel();
            return;
        case "delete":
            await this.rpc.deleteIvModuleInstance(message.instanceId);
            if (this.selectedInstanceId === message.instanceId) this.selectedInstanceId = null;
            await this.refreshModulesPanel();
            return;
        case "reveal":
            await this.revealModuleSource(message.moduleRoot);
            return;
        }
    }

    private async revealModuleSource(moduleRoot: string): Promise<void> {
        const uri = vscode.Uri.file(path.join(moduleRoot, "module.cpp"));
        const existing = vscode.window.visibleTextEditors.find(
            (editor) => editor.document.uri.toString() === uri.toString(),
        );
        if (existing) {
            await vscode.window.showTextDocument(existing.document, existing.viewColumn, false);
            return;
        }
        const existingGroup = vscode.window.tabGroups.all.find((group) =>
            group.tabs.some((tab) => {
                const input = tab.input as { uri?: vscode.Uri };
                return input.uri?.toString() === uri.toString();
            }),
        );
        const document = await vscode.workspace.openTextDocument(uri);
        if (existingGroup) {
            await vscode.window.showTextDocument(document, existingGroup.viewColumn, false);
            return;
        }
        await vscode.window.showTextDocument(document, { preview: false });
    }

    private async moduleIdForRoot(moduleRoot: string): Promise<string> {
        let source = this.ivModuleSources.find((candidate) => candidate.moduleRoot === moduleRoot);
        if (!source && this.rpc) {
            const result = await this.rpc.getIvModuleSources();
            this.ivModuleSources = this.parseIvModuleSources(result.sources);
            source = this.ivModuleSources.find((candidate) => candidate.moduleRoot === moduleRoot);
        }
        if (!source) {
            throw new Error(`module source is no longer available: ${moduleRoot}`);
        }
        return source.moduleId;
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

    private visibleInstances(): IvModuleInstanceInfo[] {
        if (!this.activeSourceFilePath) {
            return this.ivModuleInstances;
        }
        return this.ivModuleInstances;
    }

    private refreshVisibleInstances(): void {
        const instances = this.visibleInstances();
        const remembered = this.activeSourceFilePath
            ? this.selectedInstanceIdBySourceFile.get(this.activeSourceFilePath) || null
            : null;
        this.selectedInstanceId = this.resolveSelectedInstanceId(instances, remembered || this.selectedInstanceId);
        this.rememberSelectedInstance();
        this.provider.setInstances(instances);
        this.syncSelectedInstanceViews();
    }

    private rememberSelectedInstance(): void {
        const selected = this.ivModuleInstances.find((instance) => instance.instanceId === this.selectedInstanceId);
        if (this.activeSourceFilePath && selected?.instanceId) {
            this.selectedInstanceIdBySourceFile.set(this.activeSourceFilePath, selected.instanceId);
        }
    }

    private syncSelectedInstanceViews(): void {
        this.provider.setSelectedInstanceId(this.selectedInstanceId);
        this.refreshModulesPanelState();
    }

    async pausePlayback(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.pausePlayback();
        this.playbackPaused = true;
    }

    async resumePlayback(startIndex = 0): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.resumePlayback(startIndex);
        this.playbackPaused = false;
    }

    async togglePlayback(): Promise<void> {
        if (this.playbackPaused) {
            await this.resumePlayback(this.lastScrubbedSampleIndex);
        } else {
            await this.pausePlayback();
        }
    }

    async seekPlayback(sampleIndex: number): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) return;
        await this.rpc.seekPlayback(sampleIndex);
        this.lastScrubbedSampleIndex = sampleIndex;
    }

    async saveProject(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.saveProject();
    }

    async enableProjectAutosave(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.enableProjectAutosave();
    }

    async disableProjectAutosave(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        await this.rpc.disableProjectAutosave();
    }

    private laneViewRequestParams(): { viewId: string; filter: { query: string }; startIndex: number; visibleLaneCount: number } {
        const viewport = this.laneProvider.viewportState();
        return {
            viewId: this.laneViewId,
            // Keep the initial generic workspace scoped to the lanes the
            // running DSP graph contributes, without hard-coding one lane
            // subtype such as graph inputs.
            filter: { query: "dsp_graph" },
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

        const nextSourceFilePath = editor.document.uri.fsPath;
        if (nextSourceFilePath !== this.activeSourceFilePath) {
            this.activeSourceFilePath = nextSourceFilePath;
            const sourceDirectory = path.dirname(nextSourceFilePath);
            this.activeModuleRoot = path.basename(nextSourceFilePath) === "module.cpp"
                && fs.existsSync(path.join(sourceDirectory, "iv_module.json"))
                ? sourceDirectory
                : null;
            const result = await this.rpc.getIvModuleInstances(nextSourceFilePath);
            this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
            this.provider.setModuleSource(this.activeModuleRoot);
            this.refreshVisibleInstances();
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

    private showRebuildStatus(params: ServerStatusNotification): void {
        this.rebuildStatusBar.text = "$(sync~spin) Intravenous: Building module";
        this.rebuildStatusBar.tooltip = params.message || "Building updated Intravenous module definitions";
        this.rebuildStatusBar.show();
    }

    private showRebuildFailure(params: ServerStatusNotification): void {
        this.rebuildStatusBar.text = "$(error) Intravenous: Module build failed";
        this.rebuildStatusBar.tooltip = params.message || "An Intravenous module build failed";
        this.rebuildStatusBar.show();
    }

    private restartClangdForCompilationDatabase(): void {
        if (this.clangdRestartTimer) {
            clearTimeout(this.clangdRestartTimer);
        }
        this.clangdRestartTimer = setTimeout(() => {
            this.clangdRestartTimer = null;
            void vscode.commands.executeCommand("clangd.restart").then(undefined, () => {});
        }, 200);
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
        this.rebuildStatusBar.dispose();
        this.clangdDatabaseWatcher.dispose();
        if (this.clangdRestartTimer) {
            clearTimeout(this.clangdRestartTimer);
            this.clangdRestartTimer = null;
        }
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

import * as vscode from "vscode";
import * as childProcess from "child_process";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { Duplex } from "stream";

import { collectPrimarySourceSpans, QueryShape, sortNodesByRelevance } from "./graphQueryModel";
import { VirtualNode, VirtualNodeMember, VirtualPort, SourcePosition, SourceRange, SourceSpan } from "./graphModel";
import { LiveGraphControlMessage } from "./liveGraphProtocol";
import { JsonRpcSocketClient } from "./rpcClient";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { autoDetectedServerDirectoriesForWorkspaceRoot } from "./serverBinaryPaths";
import { WorkspaceNotificationRouter } from "./workspaceNotifications";
import { WorkspaceRpc } from "./workspaceRpc";
import { IvModuleInfo, ModuleInstanceInfo, ModulesControlMessage } from "./modulesViewProvider";

declare const __INTRAVENOUS_DEFAULT_DIR__: string;

const packagedDefaultServerDir = typeof __INTRAVENOUS_DEFAULT_DIR__ === "string"
    ? __INTRAVENOUS_DEFAULT_DIR__ : "";

type LiveGraphProviderLike = {
    setInstances(instances: IvModuleInstanceInfo[]): void;
    setNodes(nodes: VirtualNode[]): void;
    upsertNodes(nodes: VirtualNode[], replaceInstanceIds?: string[]): void;
    setSelectedInstanceId(instanceId: string | null): void;
    setPackageRoot(packageRoot: string | null): void;
};

type ModulesProviderLike = {
    setState(modules: IvModuleInfo[], instances: ModuleInstanceInfo[], selectedInstanceId: string | null): void;
};

type IvPackageInfo = {
    packageId: string;
    moduleIds: string[];
    nodeTypeIds: string[];
    buildState: "queued" | "building" | "built" | "failed";
    buildMessage: string;
    publicationMessage: string;
    packageRoot: string;
    projectLocal: boolean;
};

type ServerStatusNotification = {
    level?: string;
    code?: string;
    message?: string;
    packageRoot?: string;
    deletedNodeIds?: string[];
};

type ServerMessageNotification = {
    level?: string;
    message?: string;
};

type ServerReadyNotification = Record<string, never>;

type IvModuleInstanceInfo = {
    instanceId?: string;
    definitionId?: string;
    displayName?: string;
    moduleId?: string;
    packageRoot?: string;
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
    private ivPackages: IvPackageInfo[] = [];
    private selectedInstanceId: string | null = null;
    private activeSourceFilePath: string | null = null;
    private activePackageRoot: string | null = null;
    private readonly selectedInstanceIdBySourceFile = new Map<string, string>();
    private clangdRestartTimer: NodeJS.Timeout | null = null;

    constructor(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        modulesProvider: ModulesProviderLike,
        highlighter: NodeSpanHighlighter,
    ) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
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
                    this.outputChannel.appendLine(params.level === "debug"
                        ? `[debug]: ${line}` : line);
                }
            }
        });

        this.notifications.subscribe<ServerReadyNotification>("server.ready", () => {
            this.serverReadyReceived = true;
            this.outputChannel.appendLine("Intravenous server ready");
            this.resolveServerReadyWaiters();
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
            const nodes = this.parseVirtualNodes(params.nodes);
            const replaceInstanceIds = Array.isArray(params.replaceInstanceIds)
                ? params.replaceInstanceIds.filter((value): value is string => typeof value === "string")
                : [];
            // `lastQuery` is the side panel's selected source-span projection.
            // Applying a full-instance delta directly would replace that
            // projection with every port from the instance.
            if (this.lastQuery && this.rpc) {
                const refreshed = await this.refreshLastQuery();
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
                this.resetCapturedServerLogs();
                this.logServerState("Intravenous rebuild started", params);
                this.showRebuildStatus(params);
                await this.refreshModulesPanel();
                return;
            }

            if (params.code === "rebuildFinished") {
                this.logServerState("Intravenous rebuild finished", params);
                this.rebuildStatusBar.hide();
                await this.refreshModulesPanel();
                return;
            }

            if (params.code === "rebuildFailed") {
                this.logServerState("Intravenous rebuild failed", params);
                this.logCapturedServerFailureContext();
                this.showRebuildFailure(params);
                await this.refreshModulesPanel();
                return;
            }

            this.logServerState("Intravenous status", params);
        });

        this.notifications.subscribe<Record<string, never>>("ivPackages.updated", async () => {
            await this.refreshModulesPanel();
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

        // A build-and-install script packages the matching server path into
        // the extension. It must win over an old workspace setting left by a
        // previous Debug installation.
        if (packagedDefaultServerDir) {
            return {
                source: "client build default",
                path: path.join(packagedDefaultServerDir, "intravenous"),
            };
        }

        const configured = vscode.workspace
            .getConfiguration("intravenous", this.workspaceFolder.uri)
            .get<string>("intravenousDir");
        if (configured) {
            if (!path.isAbsolute(configured)) {
                throw new Error(`intravenous.intravenousDir must be absolute: ${configured}`);
            }
            return {
                source: "intravenous.intravenousDir",
                path: path.join(configured, "intravenous"),
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

        throw new Error(
            "Intravenous executable directory is not configured. " +
            "Set INTRAVENOUS_DIR or intravenous.intravenousDir, " +
            "or build the repo so the client can auto-detect build/src/intravenous.");
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

    private parseIvPackage(payload: unknown): IvPackageInfo | null {
        if (!payload || typeof payload !== "object") return null;
        const packageInfo = payload as Record<string, unknown>;
        if (typeof packageInfo.packageId !== "string" || typeof packageInfo.packageRoot !== "string") return null;
        const stringArray = (value: unknown): string[] => Array.isArray(value)
            ? value.filter((item): item is string => typeof item === "string")
            : [];
        return {
            packageId: packageInfo.packageId,
            moduleIds: stringArray(packageInfo.moduleIds),
            nodeTypeIds: stringArray(packageInfo.nodeTypeIds),
            buildState: packageInfo.buildState === "building"
                || packageInfo.buildState === "built"
                || packageInfo.buildState === "failed"
                ? packageInfo.buildState
                : "queued",
            buildMessage: typeof packageInfo.buildMessage === "string"
                ? packageInfo.buildMessage
                : "",
            publicationMessage: typeof packageInfo.publicationMessage === "string"
                ? packageInfo.publicationMessage
                : "",
            packageRoot: packageInfo.packageRoot,
            projectLocal: packageInfo.projectLocal === true,
        };
    }

    private parseIvPackageDefinitions(payload: unknown): IvPackageInfo[] {
        if (!Array.isArray(payload)) return [];
        return payload.map((packageInfo) => this.parseIvPackage(packageInfo))
            .filter((packageInfo): packageInfo is IvPackageInfo => packageInfo !== null);
    }

    private modulePanelInstances(): ModuleInstanceInfo[] {
        return this.projectModuleInstances.flatMap((instance) => {
            if (typeof instance.instanceId !== "string" || typeof instance.definitionId !== "string" || typeof instance.packageRoot !== "string") return [];
            return [{
                instanceId: instance.instanceId,
                definitionId: instance.definitionId,
                displayName: typeof instance.displayName === "string" && instance.displayName.length > 0
                    ? instance.displayName
                    : instance.definitionId,
                moduleId: instance.moduleId,
                packageRoot: instance.packageRoot,
                realized: instance.realized === true,
            }];
        });
    }

    private modulePanelModules(): IvModuleInfo[] {
        return this.ivPackages.flatMap((packageInfo) => packageInfo.moduleIds.map((moduleId) => ({
            moduleId,
            packageRoot: packageInfo.packageRoot,
            projectLocal: packageInfo.projectLocal,
        })));
    }

    private refreshModulesPanelState(): void {
        this.modulesProvider.setState(this.modulePanelModules(), this.modulePanelInstances(), this.selectedInstanceId);
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

    private parseVirtualPort(payload: unknown): VirtualPort | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        return payload as VirtualPort;
    }

    private parseVirtualNodeMember(payload: unknown): VirtualNodeMember | null {
        if (!payload || typeof payload !== "object") {
            return null;
        }
        const candidate = payload as Record<string, unknown>;
        return {
            index: typeof candidate.index === "number" ? candidate.index : undefined,
            backingNodeId: typeof candidate.backingNodeId === "string" ? candidate.backingNodeId : undefined,
            kind: typeof candidate.kind === "string" ? candidate.kind : undefined,
            typeIdentity: typeof candidate.typeIdentity === "string" ? candidate.typeIdentity : undefined,
            sampleInputs: Array.isArray(candidate.sampleInputs)
                ? candidate.sampleInputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                : undefined,
            sampleOutputs: Array.isArray(candidate.sampleOutputs)
                ? candidate.sampleOutputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                : undefined,
            eventInputs: Array.isArray(candidate.eventInputs)
                ? candidate.eventInputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                : undefined,
            eventOutputs: Array.isArray(candidate.eventOutputs)
                ? candidate.eventOutputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                : undefined,
        };
    }

    private parseVirtualNodes(payload: unknown): VirtualNode[] {
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
                ? candidate.members.map((member) => this.parseVirtualNodeMember(member)).filter((member): member is VirtualNodeMember => member !== null)
                : undefined;
            return [{
                id: typeof candidate.id === "string" ? candidate.id : undefined,
                instanceId: typeof candidate.instanceId === "string" ? candidate.instanceId : undefined,
                kind: typeof candidate.kind === "string" ? candidate.kind : undefined,
                packageIdentity: typeof candidate.packageIdentity === "string" ? candidate.packageIdentity : undefined,
                typeIdentity: typeof candidate.typeIdentity === "string" ? candidate.typeIdentity : undefined,
                sourceSpans,
                sampleInputs: Array.isArray(candidate.sampleInputs)
                    ? candidate.sampleInputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                    : undefined,
                sampleOutputs: Array.isArray(candidate.sampleOutputs)
                    ? candidate.sampleOutputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                    : undefined,
                eventInputs: Array.isArray(candidate.eventInputs)
                    ? candidate.eventInputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
                    : undefined,
                eventOutputs: Array.isArray(candidate.eventOutputs)
                    ? candidate.eventOutputs.map((port) => this.parseVirtualPort(port)).filter((port): port is VirtualPort => port !== null)
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
            if (!(await this.ensureReady()) || !this.rpc || !this.activePackageRoot) {
                return;
            }
            {
                const created = await this.rpc.createIvModuleInstance(
                    await this.moduleIdForPackageRoot(this.activePackageRoot),
                );
                this.selectedInstanceId = created.instanceId;
                this.rememberSelectedInstance();
                this.refreshVisibleInstances();
                await this.refreshActiveEditorSelection();
            }
            return;

        }
    }

    async refreshModulesPanel(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) return;
        const [packages, instances] = await Promise.all([
            this.rpc.getIvPackageDefinitions(),
            this.rpc.getIvModuleInstances(),
        ]);
        this.ivPackages = this.parseIvPackageDefinitions(packages.packages);
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
        case "createPackage": {
            const result = await this.rpc.createIvPackage(message.name);
            const packageInfo = this.parseIvPackage(result.package);
            await this.refreshModulesPanel();
            if (packageInfo) await this.revealPackageSource(packageInfo.packageRoot);
            return;
        }
        case "instantiate":
        case "duplicate": {
            const created = await this.rpc.createIvModuleInstance(
                message.moduleId,
            );
            this.selectedInstanceId = created.instanceId;
            await this.refreshModulesPanel();
            return;
        }
        case "open":
            this.selectedInstanceId = message.instanceId;
            this.syncSelectedInstanceViews();
            await this.revealPackageSource(message.packageRoot);
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
            await this.revealPackageSource(message.packageRoot);
            return;
        }
    }

    private async revealPackageSource(packageRoot: string): Promise<void> {
        const uri = vscode.Uri.file(path.join(packageRoot, "module.cpp"));
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

    private async moduleIdForPackageRoot(packageRoot: string): Promise<string> {
        let packageInfo = this.ivPackages.find((candidate) => candidate.packageRoot === packageRoot);
        if (!packageInfo && this.rpc) {
            const result = await this.rpc.getIvPackageDefinitions();
            this.ivPackages = this.parseIvPackageDefinitions(result.packages);
            packageInfo = this.ivPackages.find((candidate) => candidate.packageRoot === packageRoot);
        }
        if (!packageInfo) {
            throw new Error(`IV package is no longer available: ${packageRoot}`);
        }
        if (packageInfo.moduleIds.length !== 1) {
            const reason = packageInfo.buildState === "failed"
                ? `package build failed: ${packageInfo.buildMessage || packageRoot}`
                : packageInfo.buildState === "building" || packageInfo.buildState === "queued"
                    ? `package definitions are not ready yet: ${packageRoot}`
                    : packageInfo.publicationMessage
                        ? `package definitions are not published: ${packageInfo.publicationMessage}`
                    : `package has no published iv modules yet: ${packageRoot}`;
            throw new Error(
                packageInfo.moduleIds.length === 0
                    ? reason
                    : `package provides multiple iv modules; choose one explicitly: ${packageRoot}`,
            );
        }
        return packageInfo.moduleIds[0];
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

    updatePrimaryHighlight(nodes: VirtualNode[]): void {
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

    async updateFromEditor(editor: vscode.TextEditor | undefined): Promise<VirtualNode[]> {
        if (!this.rpc || !editor) {
            return [];
        }
        if (editor.document.uri.scheme !== "file") {
            return [];
        }

        const nextSourceFilePath = editor.document.uri.fsPath;
        if (nextSourceFilePath !== this.activeSourceFilePath) {
            this.activeSourceFilePath = nextSourceFilePath;
            const packageDirectory = path.dirname(nextSourceFilePath);
            const hasPackageManifest = fs.existsSync(path.join(packageDirectory, "iv_package.json"));
            this.activePackageRoot = path.basename(nextSourceFilePath) === "module.cpp"
                && hasPackageManifest
                ? packageDirectory
                : null;
            const result = await this.rpc.getIvModuleInstances(nextSourceFilePath);
            this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
            this.provider.setPackageRoot(this.activePackageRoot);
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

        return this.refreshLastQuery();
    }

    // Keep the side panel scoped to the most recently selected source spans
    // even when focus moves to a webview. Backend node-change notifications
    // carry instance-wide deltas, so they must be reprojected through this
    // query before updating the panel.
    private async refreshLastQuery(): Promise<VirtualNode[]> {
        if (!this.rpc || !this.lastQuery) {
            return [];
        }
        const query = this.lastQuery;
        const result = await this.rpc.queryNodesBySpans(
            query.filePath,
            query.ranges,
            query.ranges.length > 1 ? "union" : "intersection",
            this.selectedInstanceId,
        );
        const activeRegionsResult = await this.rpc.queryActiveRegions(query.filePath);
        // A newer code selection won while this RPC was in flight.
        if (this.lastQuery !== query) {
            return [];
        }
        this.lastQueryError = "";
        const nodes = sortNodesByRelevance(this.parseVirtualNodes(result.nodes), query);
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
        if (params.packageRoot) {
            parts.push(params.packageRoot);
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

import * as vscode from "vscode";
import * as childProcess from "child_process";
import * as crypto from "crypto";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

import { collectPrimarySourceSpans, QueryShape, sortNodesByRelevance } from "./graphQueryModel";
import {
    ContinuousValueAlternative,
    FractionalGlobalIndexValue,
    ScalarPresentationNumber,
    SourcePosition,
    SourceRange,
    SourceSpan,
    VirtualContinuousPort,
    VirtualEventPort,
    VirtualNode,
    VirtualNodeMember,
    VirtualPortBinding,
    VirtualPortConnectivity,
} from "./graphModel";
import { LiveGraphControlMessage } from "./liveGraphProtocol";
import { JsonRpcSocketClient } from "./rpcClient";
import {
    ProjectSessionDescriptor,
    readProjectSessionDescriptor,
} from "./projectSessionDiscovery";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { NodePresentationDescriptor } from "./nodePresentation";
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
    setNodePresentationDescriptors(descriptors: NodePresentationDescriptor[]): void;
};

type ModulesProviderLike = {
    setState(modules: IvModuleInfo[], instances: ModuleInstanceInfo[], selectedInstanceId: string | null): void;
};

type IvPackageInfo = {
    packageId: string;
    moduleIds: string[];
    nodeTypeIds: string[];
    nodePresentations: NodePresentationDescriptor[];
    buildState: "queued" | "building" | "built" | "failed";
    buildMessage: string;
    publicationMessage: string;
    packageRoot: string;
    provenance: "builtIn" | "common" | "projectLocal";
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
    configurationSource?: string;
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
    private readonly maxCapturedServerLogLines = 20;
    private serverDiagnosticLogOffset = 0;
    private readonly serverDiagnosticLogPath = process.env.IV_LOG_FILE
        || path.join(
            fs.mkdtempSync(path.join(os.tmpdir(), "intravenous-server-")),
            "diagnostics.log",
        );
    private serverReadyReceived = false;
    private lastSessionRevision = 0;
    private sessionDescriptorIdentity = "";
    private shuttingDown = false;
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

        this.notifications.subscribe<IvModuleInstancesUpdatedNotification>("nodeInstances.updated", (params) => {
            this.projectModuleInstances = Array.isArray(params.instances) ? params.instances : [];
            if (this.activeSourceFilePath && this.rpc) {
                void this.rpc.getNodeInstances(this.activeSourceFilePath).then((result) => {
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
        const nodePresentations = Array.isArray(packageInfo.nodePresentations)
            ? packageInfo.nodePresentations.flatMap((value): NodePresentationDescriptor[] => {
                if (!value || typeof value !== "object") return [];
                const descriptor = value as Record<string, unknown>;
                if (typeof descriptor.definitionId !== "string"
                    || descriptor.definitionId.length === 0
                    || typeof descriptor.presentationId !== "string"
                    || descriptor.presentationId.length === 0) {
                    return [];
                }
                return [{
                    definitionId: descriptor.definitionId,
                    presentationId: descriptor.presentationId,
                    displayName: typeof descriptor.displayName === "string"
                        ? descriptor.displayName
                        : "",
                }];
            })
            : [];
        return {
            packageId: packageInfo.packageId,
            moduleIds: stringArray(packageInfo.moduleIds),
            nodeTypeIds: stringArray(packageInfo.nodeTypeIds),
            nodePresentations,
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
            provenance: packageInfo.provenance === "builtIn"
                || packageInfo.provenance === "common"
                ? packageInfo.provenance
                : "projectLocal",
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
                configurationSource: typeof instance.configurationSource === "string"
                    ? instance.configurationSource
                    : "",
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
            provenance: packageInfo.provenance,
        })));
    }

    private refreshModulesPanelState(): void {
        this.provider.setNodePresentationDescriptors(
            this.ivPackages.flatMap((packageInfo) => packageInfo.nodePresentations),
        );
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

    private parseVirtualPortConnectivity(
        payload: unknown,
    ): VirtualPortConnectivity | null {
        return payload === "disconnected" || payload === "connected"
            || payload === "mixed" ? payload : null;
    }

    private parseVirtualPortBinding(payload: unknown): VirtualPortBinding | null {
        return payload === "interface" || payload === "none"
            || payload === "virtualInput" || payload === "virtualOutput"
            || payload === "mixed"
            ? payload : null;
    }

    private parseFractionalGlobalIndexValue(
        payload: unknown,
    ): FractionalGlobalIndexValue | null {
        if (!payload || typeof payload !== "object") return null;
        const candidate = payload as Record<string, unknown>;
        if (typeof candidate.wholeBits !== "string"
            || typeof candidate.fractionBits !== "string"
            || !/^\d+$/.test(candidate.wholeBits)
            || !/^\d+$/.test(candidate.fractionBits)) {
            return null;
        }
        const maxUint64 = (1n << 64n) - 1n;
        if (BigInt(candidate.wholeBits) > maxUint64
            || BigInt(candidate.fractionBits) > maxUint64) {
            return null;
        }
        return {
            wholeBits: candidate.wholeBits,
            fractionBits: candidate.fractionBits,
        };
    }

    private parseContinuousValueAlternative(
        payload: unknown,
    ): ContinuousValueAlternative | null {
        if (!payload || typeof payload !== "object") return null;
        const candidate = payload as Record<string, unknown>;
        if (typeof candidate.type !== "string"
            || typeof candidate.rank !== "number"
            || !Number.isSafeInteger(candidate.rank)
            || candidate.rank < 0
            || (candidate.channelCapability !== "scalar"
                && candidate.channelCapability !== "audio")) {
            return null;
        }

        const parseScalarPresentationNumber = (
            value: unknown,
        ): ScalarPresentationNumber | undefined => {
            if (typeof value === "number" && Number.isFinite(value)) return value;
            return value === "nan" || value === "positiveInfinity"
                || value === "negativeInfinity" ? value : undefined;
        };

        let defaultValue:
            ScalarPresentationNumber | FractionalGlobalIndexValue | undefined;
        const scalarDefault = parseScalarPresentationNumber(
            candidate.defaultValue);
        if (scalarDefault !== undefined) {
            defaultValue = scalarDefault;
        } else if (candidate.defaultValue !== undefined) {
            defaultValue = this.parseFractionalGlobalIndexValue(
                candidate.defaultValue) ?? undefined;
        }
        const minValue = parseScalarPresentationNumber(candidate.minValue);
        const maxValue = parseScalarPresentationNumber(candidate.maxValue);

        return {
            type: candidate.type,
            rank: candidate.rank,
            channelCapability: candidate.channelCapability,
            ...(defaultValue === undefined ? {} : { defaultValue }),
            ...(minValue === undefined ? {} : { minValue }),
            ...(maxValue === undefined ? {} : { maxValue }),
        };
    }

    private parseVirtualContinuousPort(
        payload: unknown,
    ): VirtualContinuousPort | null {
        if (!payload || typeof payload !== "object") return null;
        const candidate = payload as Record<string, unknown>;
        const connectivity = this.parseVirtualPortConnectivity(
            candidate.connectivity);
        const binding = this.parseVirtualPortBinding(candidate.binding);
        if (typeof candidate.index !== "number"
            || !Number.isSafeInteger(candidate.index)
            || candidate.index < 0
            || typeof candidate.name !== "string"
            || !connectivity
            || !binding
            || !Array.isArray(candidate.valueTypes)
            || candidate.valueTypes.length === 0) {
            return null;
        }
        const valueTypes = candidate.valueTypes.map((value) =>
            this.parseContinuousValueAlternative(value));
        if (valueTypes.some((value) => value === null)) return null;
        if (candidate.channelType !== undefined
            && typeof candidate.channelType !== "string") {
            return null;
        }
        return {
            index: candidate.index,
            name: candidate.name,
            connectivity,
            binding,
            valueTypes: valueTypes as ContinuousValueAlternative[],
            ...(typeof candidate.channelType === "string"
                ? { channelType: candidate.channelType } : {}),
        };
    }

    private parseVirtualEventPort(payload: unknown): VirtualEventPort | null {
        if (!payload || typeof payload !== "object") return null;
        const candidate = payload as Record<string, unknown>;
        const connectivity = this.parseVirtualPortConnectivity(
            candidate.connectivity);
        const binding = this.parseVirtualPortBinding(candidate.binding);
        if (typeof candidate.index !== "number"
            || !Number.isSafeInteger(candidate.index)
            || candidate.index < 0
            || typeof candidate.name !== "string"
            || typeof candidate.eventType !== "string"
            || !connectivity
            || !binding) {
            return null;
        }
        return {
            index: candidate.index,
            name: candidate.name,
            connectivity,
            binding,
            eventType: candidate.eventType,
        };
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
            definitionId: typeof candidate.definitionId === "string" ? candidate.definitionId : undefined,
            continuousInputs: Array.isArray(candidate.continuousInputs)
                ? candidate.continuousInputs.map((port) => this.parseVirtualContinuousPort(port)).filter((port): port is VirtualContinuousPort => port !== null)
                : undefined,
            continuousOutputs: Array.isArray(candidate.continuousOutputs)
                ? candidate.continuousOutputs.map((port) => this.parseVirtualContinuousPort(port)).filter((port): port is VirtualContinuousPort => port !== null)
                : undefined,
            eventInputs: Array.isArray(candidate.eventInputs)
                ? candidate.eventInputs.map((port) => this.parseVirtualEventPort(port)).filter((port): port is VirtualEventPort => port !== null)
                : undefined,
            eventOutputs: Array.isArray(candidate.eventOutputs)
                ? candidate.eventOutputs.map((port) => this.parseVirtualEventPort(port)).filter((port): port is VirtualEventPort => port !== null)
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
                definitionId: typeof candidate.definitionId === "string" ? candidate.definitionId : undefined,
                sourceSpans,
                continuousInputs: Array.isArray(candidate.continuousInputs)
                    ? candidate.continuousInputs.map((port) => this.parseVirtualContinuousPort(port)).filter((port): port is VirtualContinuousPort => port !== null)
                    : undefined,
                continuousOutputs: Array.isArray(candidate.continuousOutputs)
                    ? candidate.continuousOutputs.map((port) => this.parseVirtualContinuousPort(port)).filter((port): port is VirtualContinuousPort => port !== null)
                    : undefined,
                eventInputs: Array.isArray(candidate.eventInputs)
                    ? candidate.eventInputs.map((port) => this.parseVirtualEventPort(port)).filter((port): port is VirtualEventPort => port !== null)
                    : undefined,
                eventOutputs: Array.isArray(candidate.eventOutputs)
                    ? candidate.eventOutputs.map((port) => this.parseVirtualEventPort(port)).filter((port): port is VirtualEventPort => port !== null)
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

        const existing = readProjectSessionDescriptor(this.workspaceRoot());
        if (existing && await this.attachToProjectSession(existing)) {
            await this.loadInitialSessionState();
            return true;
        }

        const binary = this.serverBinaryPath();
        const serverDir = path.dirname(binary);
        const args = [
            "--server",
            "--workspace-root",
            this.workspaceRoot(),
        ];
        const childEnv = {
            ...process.env,
            INTRAVENOUS_DIR: process.env.INTRAVENOUS_DIR || serverDir,
            IV_LOG_FILE: this.serverDiagnosticLogPath,
        };

        this.resetCapturedServerLogs();
        this.outputChannel.appendLine(`starting Intravenous server: ${binary}`);
        this.outputChannel.appendLine(`Intravenous server cwd: ${this.workspaceRoot()}`);
        this.outputChannel.appendLine(
            `Intravenous server diagnostic log: ${this.serverDiagnosticLogPath}`,
        );
        const processLog = fs.openSync(this.serverDiagnosticLogPath, "a");
        try {
            this.process = childProcess.spawn(binary, args, {
                cwd: this.workspaceRoot(),
                env: childEnv,
                detached: true,
                stdio: ["ignore", processLog, processLog],
            });
        } finally {
            fs.closeSync(processLog);
        }
        const spawnedProcess = this.process;
        spawnedProcess.unref();
        this.outputChannel.appendLine("Intravenous startup: server process spawned");

        spawnedProcess.on("error", (error) => {
            this.outputChannel.appendLine(`Intravenous server spawn failed: ${error.message}`);
        });
        spawnedProcess.on("exit", (code, signal) => {
            if (this.process === spawnedProcess) this.process = null;
            this.rebuildStatusBar.hide();
            this.outputChannel.appendLine(`Intravenous server exited: code=${code} signal=${signal}`);
        });

        if (!(await this.attachToAvailableProjectSession(10000))) {
            this.logCapturedServerFailureContext();
            throw new Error(
                "Intravenous project session did not accept a connection before timeout",
            );
        }
        await this.loadInitialSessionState();
        return true;
    }

    private async attachToProjectSession(
        descriptor: ProjectSessionDescriptor,
    ): Promise<boolean> {
        this.resetServerReadyState();
        let attached = false;
        const descriptorIdentity =
            `${descriptor.pid}:${descriptor.startupTimeUnixMs}:${descriptor.localCapability}`;
        const afterRevision = descriptorIdentity === this.sessionDescriptorIdentity
            ? this.lastSessionRevision
            : 0;
        let client: JsonRpcSocketClient;
        client = new JsonRpcSocketClient(
            descriptor.endpoint.path,
            (method, params) => {
                if (typeof params.revision === "number") {
                    this.lastSessionRevision = Math.max(
                        this.lastSessionRevision, params.revision);
                }
                void this.notifications.dispatch(method, params);
            },
            (error) => {
                if (attached) this.handleProjectSessionDisconnect(client, error);
            },
            (revision) => {
                this.lastSessionRevision = revision;
            },
        );
        try {
            this.outputChannel.appendLine(
                `Intravenous startup: attaching to project session ${descriptor.endpoint.path}`,
            );
            await client.connect(1000);
            this.client = client;
            this.rpc = new WorkspaceRpc(client, () => this.lastSessionRevision);
            await this.waitForServerReady(3000);
            await this.rpc.hello(
                `vscode-${process.pid}-${crypto.randomUUID()}`,
                descriptor.localCapability,
            );
            const subscription = await this.rpc.subscribe(afterRevision);
            this.lastSessionRevision = subscription.currentRevision;
            if (subscription.snapshotRequired) {
                const snapshot = await this.rpc.snapshot();
                this.applySessionSnapshot(snapshot);
                if (typeof snapshot.sessionRevision === "number") {
                    this.lastSessionRevision = snapshot.sessionRevision;
                }
                const resumed = await this.rpc.subscribe(this.lastSessionRevision);
                this.lastSessionRevision = resumed.currentRevision;
                if (resumed.snapshotRequired) {
                    throw new Error(
                        "project session replay window advanced while applying a snapshot",
                    );
                }
            }
            this.outputChannel.appendLine(
                `Intravenous startup: attached at session revision ${subscription.currentRevision}`,
            );
            this.sessionDescriptorIdentity = descriptorIdentity;
            attached = true;
            return true;
        } catch (error) {
            attached = false;
            client.dispose();
            if (this.client === client) this.client = null;
            this.rpc = null;
            this.outputChannel.appendLine(
                `Intravenous startup: project session attachment failed: ${error instanceof Error ? error.message : String(error)}`,
            );
            return false;
        }
    }

    private handleProjectSessionDisconnect(
        client: JsonRpcSocketClient,
        error: Error,
    ): void {
        if (this.shuttingDown || this.client !== client) return;
        this.outputChannel.appendLine(
            `Intravenous project session disconnected: ${error.message}; reconnecting`,
        );
        this.client = null;
        this.rpc = null;
        this.resetServerReadyState();
        const priorStart = this.startInFlight;
        void (async () => {
            if (priorStart) {
                try {
                    await priorStart;
                } catch {
                }
            }
            while (!this.shuttingDown && !this.rpc) {
                try {
                    await this.start();
                } catch (reconnectError) {
                    this.outputChannel.appendLine(
                        `Intravenous project session reconnect attempt failed: ${reconnectError instanceof Error ? reconnectError.message : String(reconnectError)}`,
                    );
                }
                if (!this.shuttingDown && !this.rpc) {
                    await new Promise<void>((resolve) => setTimeout(resolve, 1000));
                }
            }
        })().catch((reconnectError) => {
                this.outputChannel.appendLine(
                    `Intravenous project session reconnect failed: ${reconnectError instanceof Error ? reconnectError.message : String(reconnectError)}`,
                );
            });
    }

    private async attachToAvailableProjectSession(
        timeoutMs: number,
    ): Promise<boolean> {
        const deadline = Date.now() + timeoutMs;
        let lastEndpoint = "";
        for (;;) {
            const descriptor = readProjectSessionDescriptor(this.workspaceRoot());
            if (descriptor) {
                if (descriptor.endpoint.path !== lastEndpoint) {
                    lastEndpoint = descriptor.endpoint.path;
                    this.outputChannel.appendLine(
                        `Intravenous startup: discovered project session ${lastEndpoint}`,
                    );
                }
                if (await this.attachToProjectSession(descriptor)) return true;
            }
            if (Date.now() >= deadline) return false;
            await new Promise<void>((resolve) => setTimeout(resolve, 50));
        }
    }

    private async loadInitialSessionState(): Promise<void> {
        if (!this.rpc) return;
        const result = await this.rpc.getNodeInstances();
        this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
        this.projectModuleInstances = this.ivModuleInstances;
        this.refreshVisibleInstances();
        const realizedCount = this.ivModuleInstances.filter(
            (instance) => instance.realized,
        ).length;
        this.outputChannel.appendLine(
            `Intravenous instances after ready: total=${this.ivModuleInstances.length} realized=${realizedCount} selected=${this.selectedInstanceId ?? "[none]"}`,
        );
    }

    private applySessionSnapshot(snapshot: Record<string, unknown>): void {
        const nodeInstances = snapshot.nodeInstances as
            | { instances?: unknown[] }
            | undefined;
        if (Array.isArray(nodeInstances?.instances)) {
            this.ivModuleInstances = this.parseIvModuleInstances(
                nodeInstances.instances);
            this.projectModuleInstances = this.ivModuleInstances;
            this.refreshVisibleInstances();
        }
        const packages = snapshot.packages as
            | { packages?: unknown[] }
            | undefined;
        if (Array.isArray(packages?.packages)) {
            this.ivPackages = this.parseIvPackageDefinitions(packages.packages);
            this.refreshModulesPanelState();
        }
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
                const created = await this.rpc.createNodeInstance(
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
            this.rpc.getNodeInstances(),
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
            const created = await this.rpc.createNodeInstance(
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
            await this.rpc.updateNodeInstances([{
                instanceId: message.instanceId,
                displayName: message.displayName,
            }]);
            await this.refreshModulesPanel();
            return;
        case "delete":
            await this.rpc.deleteNodeInstance(message.instanceId);
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
            const result = await this.rpc.getNodeInstances(nextSourceFilePath);
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
        try {
            this.serverDiagnosticLogOffset = fs.statSync(
                this.serverDiagnosticLogPath).size;
        } catch {
            this.serverDiagnosticLogOffset = 0;
        }
    }

    private logCapturedServerFailureContext(): void {
        let text = "";
        try {
            text = fs.readFileSync(this.serverDiagnosticLogPath, "utf8")
                .slice(this.serverDiagnosticLogOffset);
        } catch {
            return;
        }
        const lines = text.split(/\r?\n/)
            .map((line) => line.trim())
            .filter((line) => line.length > 0)
            .slice(-this.maxCapturedServerLogLines);
        if (lines.length > 0) {
            this.outputChannel.appendLine("Intravenous server failure details:");
            for (const line of lines) this.outputChannel.appendLine(`  ${line}`);
        }
        this.resetCapturedServerLogs();
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
        this.shuttingDown = true;
        this.rebuildStatusBar.dispose();
        this.clangdDatabaseWatcher.dispose();
        if (this.clangdRestartTimer) {
            clearTimeout(this.clangdRestartTimer);
            this.clangdRestartTimer = null;
        }
        if (this.client) {
            this.client.dispose();
            this.client = null;
        }
        this.rpc = null;
        // A client does not own the project session merely because it won the
        // create-or-connect race and started the server process.
        this.process = null;
    }
}

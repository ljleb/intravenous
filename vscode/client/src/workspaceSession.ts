import type { ObservationInterest } from "./observations";
import type { ObservationDelivery } from "./observationDelivery";
import { ManualControlInteractions, resolveManualControl, type ManualControlId } from "./manualControls";
import { ControlCompilationProgress, type ControlCompileStatus } from "./controlCompilation";
import { addressFromKey, addressKey, graphScopeWithChildren, laneViewFromKey, type GraphLaneCopy, type GraphScope, type NodeAddress } from "./graphScope";
import type { GraphPresentationPatch } from "./graphEditorProtocol";
import { insertLanes, mergeLaneOrder, orderLanes } from "./laneSelection";
import { validateNodeEventTarget, type SerializedNodeEvent } from "./nodeEvents";
import { NodeEventInteractions } from "./nodeEventInteractions";
import type { PlaybackDrag, PlaybackSnapshot } from "./playback";
import type { NodeDefinitionCatalog, NodeDefinitionSearch } from "./nodeDefinitionSearch";
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
import { LiveGraphControlMessage, LiveGraphSessionStatus } from "./liveGraphProtocol";
import { Duplex } from "stream";

import { JsonRpcSocketClient } from "./rpcClient";
import { connectionEndMutation, connectionFromPorts, type GraphConnectionEdit, type GraphPortEndpoint } from "./graphConnectionEditing";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { resolveIntravenousBinary } from "./serverBinaryPaths";
import { WorkspaceNotificationRouter } from "./workspaceNotifications";
import { WorkspaceRpc } from "./workspaceRpc";
import { audioOutputDiagnostic } from "./audioOutputDiagnostic";
import { statusBarText } from "./statusBarText";
import { isGeneratedSnapConnection, isTimelineSnapInstance, isTimelineSnapNode, planTimelineSnapMode, readTimelineSnapMemory,
    type ProjectEditOperation, type TimelineGridMode } from "./timelineSnap";

function laneOrderOperation(order: readonly string[]): ProjectEditOperation {
    return { method: "projectPresentation.setLaneOrder", params: { order: order.map(addressFromKey) } };
}

function orderWithInsertedLanes(scope: GraphScope, insertions: readonly { instanceId: string; afterInstanceId?: string }[]) {
    const saved = scope.presentation?.laneOrder.map(addressKey) ?? [];
    const visible = orderLanes(scope.nodes.filter((node) => !isTimelineSnapNode(node)), saved);
    const order = insertLanes(visible.map(({ address }) => addressKey(address)),
        insertions.map(({ instanceId, afterInstanceId }) => ({
            key: addressKey({ instanceId }),
            after: afterInstanceId === undefined ? undefined : addressKey({ instanceId: afterInstanceId }),
        })));
    return laneOrderOperation(mergeLaneOrder(saved, order));
}

type LiveGraphProviderLike = {
    setSessionStatus(status: LiveGraphSessionStatus): void;
    setInstances(instances: IvModuleInstanceInfo[]): void;
    setNodes(nodes: VirtualNode[]): void;
    upsertNodes(nodes: VirtualNode[], replaceInstanceIds?: string[]): void;
    setSelectedInstanceId(instanceId: string | null): void;
    setPackageRoot(packageRoot: string | null): void;
};

type IvPackageInfo = {
    packageId: string;
    moduleIds: string[];
    nodeTypeIds: string[];
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

type ServerReadyNotification = {
    sessionRevision?: number;
};

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
    async readPlayback(): Promise<PlaybackSnapshot | null> {
        const rpc = this.rpc;
        if (!rpc || this.graphSessionStatus !== "running") return null;
        const state = await rpc.getPlayback();
        return this.rpc === rpc && this.graphSessionStatus === "running" ? state : null;
    }
    async playbackCommand(position?: string, drag?: PlaybackDrag): Promise<PlaybackSnapshot | null> {
        const rpc = this.rpc;
        if (!rpc || this.graphSessionStatus !== "running") throw new Error("The server is unavailable");
        const snapshot = await (position === undefined ? rpc.togglePlayback()
            : drag ? rpc.dragPlayback(position, drag) : rpc.seekPlayback(position));
        return this.rpc === rpc && this.graphSessionStatus === "running" ? snapshot : null;
    }
    private readonly observationChanges = new vscode.EventEmitter<ObservationDelivery>();
    readonly onDidChangeObservations = this.observationChanges.event;
    private readonly graphChanges = new vscode.EventEmitter<void>();
    readonly onDidChangeGraph = this.graphChanges.event;
    graphSessionStatus: LiveGraphSessionStatus = "starting";
    private readonly manualChanges = new vscode.EventEmitter<void>();
    readonly onDidChangeManualControls = this.manualChanges.event;
    readonly manualControls = new ManualControlInteractions(() => this.rpc, () => this.manualChanges.fire());
    readonly nodeEvents = new NodeEventInteractions({
        begin: (target, id) => this.nodeEventGesture(target, id, true),
        send: (target, event, id) => this.sendNodeEvent(target, event, id),
        end: (target, id) => this.nodeEventGesture(target, id, false),
        report: (error) => this.outputChannel.appendLine(`Node gesture cleanup failed: ${String(error)}`),
    });

    private async nodeEventGesture(target: NodeAddress, id: string, begin: boolean): Promise<number> {
        if (this.graphSessionStatus !== "running" || !this.rpc) return Promise.reject(new Error("The server is unavailable"));
        await this.rpc.nodeEventGesture(target, id, begin);
        return this.lastSessionRevision;
    }

    get projectFolder(): vscode.Uri { return this.workspaceFolder.uri; }

    async describeObservations(nodes: NodeAddress[]) {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        return this.rpc.describeObservations(nodes);
    }
    async reconcileObservations(scopeId: string, interests: ObservationInterest[], predictedInterests?: ObservationInterest[]) {
        if (!this.rpc || this.graphSessionStatus !== "running") throw new Error("The server is unavailable");
        return this.rpc.reconcileObservations(scopeId, interests, predictedInterests);
    }

    async readGraphScope(expanded: NodeAddress[] = []) {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        return this.rpc.getGraphScope(expanded);
    }

    private timelineSnapChange: Promise<void> = Promise.resolve();
    private presentationChanges: Promise<unknown> = Promise.resolve();

    saveGraphPresentation(patch: GraphPresentationPatch): Promise<{ sessionRevision: number }> {
        const rpc = this.rpc;
        const change = this.presentationChanges.catch(() => {}).then(async () => {
            if (!rpc || rpc !== this.rpc || this.graphSessionStatus !== "running") throw new Error("The server is unavailable");
            const operations: ProjectEditOperation[] = Object.entries(patch.presentationStates)
                .map(([key, state]) => ({ method: "projectPresentation.setLaneView", params: laneViewFromKey(key, state) }));
            if (patch.laneOrder) operations.push(laneOrderOperation(patch.laneOrder));
            return rpc.applyProjectEdit(operations, patch.laneOrder ? undefined : null);
        });
        this.presentationChanges = change;
        return change;
    }

    setTimelineSnapMode(mode: TimelineGridMode): Promise<void> {
        const change = this.timelineSnapChange.catch(() => {}).then(async () => {
            await this.presentationChanges.catch(() => {});
            if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
            const rpc = this.rpc;
            const revision = this.lastSessionRevision;
            const scope = await rpc.getGraphScope();
            const memory = readTimelineSnapMemory(scope.presentation?.timelineSnapMemory);
            const { operations, memory: nextMemory } = planTimelineSnapMode(scope, mode, memory, crypto.randomUUID);
            if (rpc !== this.rpc) throw new Error("The session changed before editing the snap mode");
            if (operations.length) await rpc.applyProjectEdit([...operations,
                { method: "projectPresentation.setTimelineSnapMemory", params: { memory: nextMemory } },
            ], scope.sessionRevision ?? revision);
        });
        this.timelineSnapChange = change;
        return change;
    }

    async setManualControlEnabled(control: ManualControlId, enabled: boolean, expanded: NodeAddress[]): Promise<void> {
        if (control.inputName === undefined) throw new Error("Only input knobs have an enable setting");
        const rpc = this.rpc;
        if (!rpc || this.graphSessionStatus !== "running") throw new Error("The server is unavailable");
        await this.manualControls.settled();
        const scope = graphScopeWithChildren(await rpc.getGraphScope(expanded));
        if (rpc !== this.rpc) throw new Error("The session changed before editing the input");
        const descriptor = resolveManualControl(scope, control);
        if (!descriptor?.available || descriptor.live) throw new Error("The manual input is unavailable");
        if (descriptor.enabled === enabled) return;
        await rpc.setManualInputEnabled(control, descriptor.value, enabled, descriptor.participation, scope.sessionRevision);
    }

    async sendNodeEvent(target: NodeAddress, event: SerializedNodeEvent, interactionId?: string): Promise<number> {
        // A semantic edit belongs to the running session that received it.
        // Do not hold it across startup/restart and apply it to a later graph.
        if (this.graphSessionStatus !== "running" || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        const revision = this.lastSessionRevision;
        const protocol = await rpc.getNodeEventProtocol();
        if (this.rpc !== rpc) throw new Error("The session changed before event dispatch");
        validateNodeEventTarget(protocol, target, event);
        // Standalone edits still use the revision from before the protocol
        // read. An open gesture instead uses the server's conflict check for
        // its target and history group. Never retry a rejected event.
        const result = await rpc.dispatchNodeEvent(target, event, revision, interactionId);
        if (!result.ok || result.diagnostics.length)
            throw new Error(result.diagnostics.map((entry) => entry.message).join("\n") || "Node event was not accepted");
        return result.sessionRevision ?? this.lastSessionRevision;
    }

    async connectGraphPorts(id: string, first: GraphPortEndpoint, second: GraphPortEndpoint, expanded: NodeAddress[]): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        const revision = this.lastSessionRevision;
        const scope = graphScopeWithChildren(await rpc.getGraphScope(expanded));
        const connection = connectionFromPorts(scope, id, first, second);
        await rpc.upsertGraphConnection(connection, scope.sessionRevision ?? revision);
    }

    async deleteGraphConnection(id: string): Promise<void> {
        const scope = await this.readGraphScope();
        if (!scope.editable || !scope.connections.some((entry) => entry.id === id && !isGeneratedSnapConnection(entry))) {
            throw new Error("The selected connection is no longer available");
        }
        await this.rpc!.deleteGraphConnection(id);
    }

    async editGraphConnectionEnd(edit: GraphConnectionEdit, expanded: NodeAddress[]): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        // Bind the mutation to a revision from before the read. A notification
        // arriving between the read and the write must not validate an old edit.
        const revision = this.lastSessionRevision;
        const mutation = connectionEndMutation(graphScopeWithChildren(await rpc.getGraphScope(expanded)), edit);
        if (mutation?.type === "delete") await rpc.deleteGraphConnection(mutation.id, revision);
        else if (mutation?.type === "upsert") await rpc.upsertGraphConnection(mutation.connection, revision);
    }

    private definitionCatalog?: { rpc: WorkspaceRpc; pending: Promise<NodeDefinitionCatalog> };

    async graphDefinitions(): Promise<NodeDefinitionCatalog> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        if (this.definitionCatalog?.rpc === this.rpc) return this.definitionCatalog.pending;
        const catalog = { rpc: this.rpc, pending: this.rpc.nodeDefinitionCatalog() };
        this.definitionCatalog = catalog;
        try { return await catalog.pending; }
        catch (error) {
            if (this.definitionCatalog === catalog) this.definitionCatalog = undefined;
            throw error;
        }
    }

    async searchNodeDefinitions(source: string, cursor: number): Promise<NodeDefinitionSearch> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        return this.rpc.searchNodeDefinitions(source, cursor);
    }

    async createGraphNode(definitionId: string, displayName: string, configurationSource: string, afterInstanceId?: string): Promise<string> {
        await this.presentationChanges.catch(() => {});
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        const scope = await rpc.getGraphScope();
        if (rpc !== this.rpc) throw new Error("The session changed before creating the lane");
        const instanceId = crypto.randomUUID();
        await rpc.applyProjectEdit([
            { method: "nodeInstances.create", params: { instanceId, definitionId, displayName: displayName.trim() || null, configurationSource } },
            orderWithInsertedLanes(scope, [{ instanceId, afterInstanceId }]),
        ], scope.sessionRevision);
        return instanceId;
    }

    async updateGraphNode(instanceId: string, displayName: string, previousDisplayName: string,
        configuration?: { source: string; previous: string }): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        const scope = await rpc.getGraphScope();
        if (rpc !== this.rpc) throw new Error("The session changed before saving the node");
        const node = scope.nodes.find((entry) => entry.instance.instanceId === instanceId);
        if (!scope.editable || !node || isTimelineSnapNode(node)) throw new Error("The selected node is no longer available");
        if ((node.instance.displayName ?? "") !== previousDisplayName) {
            throw new Error("The node name changed. Start the edit again to use the current name.");
        }
        if (configuration && (node.instance.configurationSource ?? "") !== configuration.previous) {
            throw new Error("The node configuration changed. Reopen the editor before saving.");
        }
        const nameChanged = displayName !== previousDisplayName;
        const configurationChanged = configuration && configuration.source !== configuration.previous;
        if (!nameChanged && !configurationChanged) return;
        await rpc.updateNodeInstances([{ instanceId,
            ...(nameChanged ? { displayName: displayName.trim() } : {}),
            ...(configurationChanged ? { configurationSource: configuration.source } : {}),
        }], scope.sessionRevision);
    }

    async deleteGraphNodes(instanceIds: string[]): Promise<void> {
        await this.presentationChanges.catch(() => {});
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        const revision = this.lastSessionRevision;
        const scope = await rpc.getGraphScope();
        if (!scope.editable || instanceIds.some((id) => !scope.nodes.some((node) => node.instance.instanceId === id && !isTimelineSnapNode(node)))) {
            throw new Error("A selected node is no longer available");
        }
        if (!instanceIds.length) return;
        await rpc.applyProjectEdit(instanceIds.map((instanceId) => ({ method: "nodeInstances.delete", params: { instanceId } })), revision);
    }

    async duplicateGraphNodes(copies: readonly GraphLaneCopy[]): Promise<void> {
        if (!copies.length) return;
        await this.presentationChanges.catch(() => {});
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const rpc = this.rpc;
        await this.manualControls.settled();
        const revision = this.lastSessionRevision;
        const scope = await rpc.getGraphScope();
        if (rpc !== this.rpc) throw new Error("The session changed before duplicating the lanes");
        const visible = new Set(scope.nodes.filter((node) => !isTimelineSnapNode(node)).map(({ instance }) => instance.instanceId));
        if (!scope.editable || copies.some(({ sourceInstanceId }) => !visible.has(sourceInstanceId))) {
            throw new Error("A selected node is no longer available");
        }
        await rpc.applyProjectEdit([
            { method: "nodeInstances.duplicate", params: { copies } },
            orderWithInsertedLanes(scope, copies.map(({ sourceInstanceId, instanceId }) => ({ instanceId, afterInstanceId: sourceInstanceId }))),
        ], scope.sessionRevision ?? revision);
    }

    async projectHistory(direction: "undo" | "redo"): Promise<void> {
        await this.presentationChanges.catch(() => {});
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        await this.manualControls.settled();
        await this.rpc.projectHistory(direction);
    }

    async openGraphNodeSource(instanceId: string): Promise<void> {
        const scope = await this.readGraphScope();
        const instance = scope.nodes.find((entry) => entry.instance.instanceId === instanceId)?.instance;
        if (!instance?.packageRoot) throw new Error("The node's package source is unavailable");
        this.selectedInstanceId = instanceId;
        this.syncSelectedInstanceViews();
        await this.revealPackageSource(instance.packageRoot);
    }

    async createGraphPackage(name: string): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) throw new Error("The server is unavailable");
        const result = await this.rpc.createIvPackage(name);
        const info = this.parseIvPackage(result.package);
        if (info) await this.revealPackageSource(info.packageRoot);
    }
    private readonly workspaceFolder: vscode.WorkspaceFolder;
    private readonly outputChannel: vscode.OutputChannel;
    readonly provider: LiveGraphProviderLike;
    private readonly highlighter: NodeSpanHighlighter;
    private readonly rebuildStatusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    private readonly controlCompilationProgress = new ControlCompilationProgress();
    private readonly controlCompilationStatus = vscode.window.createStatusBarItem(
        "intravenous.controlCompilation", vscode.StatusBarAlignment.Left, 99);
    private controlCompilationTimer: ReturnType<typeof setTimeout> | undefined;
    private readonly clangdDatabaseWatcher: vscode.FileSystemWatcher;
    private readonly notifications = new WorkspaceNotificationRouter();
    private process: childProcess.ChildProcess | null = null;
    private client: JsonRpcSocketClient | null = null;
    private rpc: WorkspaceRpc | null = null;
    lastQueryError = "";
    private lastQuery: QueryShape | null = null;
    private startInFlight: Promise<boolean> | null = null;
    private lastTerminalStatusMessage = "";
    private serverDiagnosticLogOffset = 0;
    private readonly serverDiagnosticLogPath = process.env.IV_LOG_FILE
        || path.join(
            fs.mkdtempSync(path.join(os.tmpdir(), "intravenous-server-")),
            "diagnostics.log",
        );
    private serverReadyReceived = false;
    // Covers package/project loading before the server starts serving.
    private readonly serverReadyTimeoutMs = 20000;
    private lastSessionRevision = 0;
    // Restart backoff for a server that keeps failing soon after it starts.
    private readonly initialRestartDelayMs = 1000;
    private readonly stableServerRunMs = 30000;
    private readonly maximumRestartAttempts = 3;
    private restartDelayMs = 1000;
    private restartAttempts = 0;
    private restartInFlight = false;
    private selectedServerBinary: string | null = null;
    private connectedAtMs: number | null = null;
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
        highlighter: NodeSpanHighlighter,
        private readonly bundledRuntime?: string,
    ) {
        this.workspaceFolder = workspaceFolder;
        this.outputChannel = outputChannel;
        this.provider = provider;
        this.highlighter = highlighter;
        this.clangdDatabaseWatcher = vscode.workspace.createFileSystemWatcher(
            new vscode.RelativePattern(this.workspaceFolder, "compile_commands.json"),
        );
        this.clangdDatabaseWatcher.onDidCreate(() => this.restartClangdForCompilationDatabase());
        this.clangdDatabaseWatcher.onDidChange(() => this.restartClangdForCompilationDatabase());
        this.registerNotificationHandlers();
    }

    private registerNotificationHandlers(): void {
        this.notifications.subscribe<ControlCompileStatus>("graph.controlCompilation", (status) => {
            const progress = this.controlCompilationProgress.receive(status);
            this.outputChannel.appendLine(progress.text);
            if (this.controlCompilationTimer) clearTimeout(this.controlCompilationTimer);
            this.controlCompilationStatus.text = progress.failed ? "$(error) IV compile failed · See Output"
                : progress.queued || progress.compiling
                    ? `$(sync~spin) IV ${progress.compiling} compiling · ${progress.queued} queued`
                    : `$(check) IV ${progress.summary}`;
            this.controlCompilationStatus.show();
            if (!progress.queued && !progress.compiling && !progress.failed) {
                this.controlCompilationTimer = setTimeout(() => this.controlCompilationStatus.hide(), 3000);
            }
        });
        this.notifications.subscribe<ObservationDelivery>("observations.updated", (update) => this.observationChanges.fire(update));
        this.notifications.subscribe("graph.changed", () => this.graphChanges.fire());
        // A committed revision without a graph publication, such as a settled
        // manual value or an audio selection.
        this.notifications.subscribe("session.revisionChanged", () => this.graphChanges.fire());
        // Another client's open gesture: unreliable, display only.
        this.notifications.subscribe<{ node?: NodeAddress; inputName?: string; outputName?: string; value?: number }>(
            "manualControls.liveValue", (params) => {
                if (!params.node || typeof params.value !== "number") return;
                if (typeof params.outputName === "string" && params.inputName === undefined)
                    this.manualControls.receiveLive({ control: { node: params.node, outputName: params.outputName }, value: params.value });
                else if (typeof params.inputName === "string" && params.outputName === undefined)
                    this.manualControls.receiveLive({ control: { node: params.node, inputName: params.inputName }, value: params.value });
            });
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

        this.notifications.subscribe<ServerReadyNotification>("server.ready", (params) => {
            if (typeof params.sessionRevision === "number") {
                this.lastSessionRevision = params.sessionRevision;
            }
            this.serverReadyReceived = true;
            this.outputChannel.appendLine("Server ready");
            this.resolveServerReadyWaiters();
        });

        this.notifications.subscribe<IvModuleInstancesUpdatedNotification>("nodeInstances.updated", (params) => {
            this.projectModuleInstances = Array.isArray(params.instances) ? params.instances : [];
            if (this.activeSourceFilePath && this.rpc) {
                void this.rpc.getNodeInstances(this.activeSourceFilePath).then((result) => {
                    this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
                    this.refreshVisibleInstances();
                });
                return;
            }
            this.ivModuleInstances = this.projectModuleInstances;
            this.refreshVisibleInstances();
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
                this.lastTerminalStatusMessage = params.message || "Startup failed";
                this.logServerState("Startup failed", params);
                return;
            }

            if (params.code === "rebuildStarted") {
                this.resetCapturedServerLogs();
                this.logServerState("Rebuild started", params);
                this.showRebuildStatus(params);
                await this.refreshProjectCatalog();
                return;
            }

            if (params.code === "rebuildFinished") {
                this.logServerState("Rebuild finished", params);
                this.rebuildStatusBar.hide();
                await this.refreshProjectCatalog();
                return;
            }

            if (params.code === "rebuildFailed" || params.code === "graphCompilationFailed") {
                this.logCapturedServerFailureContext();
                // Keep the actionable failure below compiler output and traces.
                this.logServerState("Rebuild failed", params);
                this.showRebuildFailure();
                await this.refreshProjectCatalog();
                return;
            }

            this.logServerState("Status", params);
        });

        this.notifications.subscribe<Record<string, never>>("ivPackages.updated", async () => {
            // A failed build can change node availability without producing a
            // graph generation or graph.changed notification.
            this.definitionCatalog = undefined;
            this.graphChanges.fire();
            await this.refreshProjectCatalog();
        });
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
        const scrubSpeed = parseScalarPresentationNumber(candidate.scrubSpeed);

        return {
            type: candidate.type,
            rank: candidate.rank,
            channelCapability: candidate.channelCapability,
            ...(defaultValue === undefined ? {} : { defaultValue }),
            ...(minValue === undefined ? {} : { minValue }),
            ...(maxValue === undefined ? {} : { maxValue }),
            ...(scrubSpeed === undefined ? {} : { scrubSpeed }),
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
                sourceIdentity: typeof candidate.sourceIdentity === "string" ? candidate.sourceIdentity : undefined,
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
        if (this.selectedServerBinary) return this.selectedServerBinary;
        const configuredDir = vscode.workspace.getConfiguration("intravenous", this.workspaceFolder.uri).get<string>("intravenousDir");
        const resolved = resolveIntravenousBinary("intravenous", configuredDir, this.bundledRuntime);
        try {
            if (!fs.statSync(resolved.path).isFile()) throw new Error("server executable is not a file");
            fs.accessSync(resolved.path, fs.constants.X_OK);
        } catch (error) {
            throw new Error(`Cannot use the server from ${resolved.source} at ${resolved.path}: ${error.message}`);
        }
        this.selectedServerBinary = resolved.path;
        this.outputChannel.appendLine(`Resolved server from ${resolved.source}: ${resolved.path}`);
        return resolved.path;
    }

    async start(): Promise<boolean> {
        if (this.startInFlight) return this.startInFlight;
        if (this.rpc) {
            return true;
        }
        if (this.shuttingDown) return false;
        this.startInFlight = this.startImpl().catch((error) => {
            if (!this.restartInFlight && !this.shuttingDown && this.graphSessionStatus !== "restarting")
                this.stopRestarting(error.message);
            throw error;
        }).finally(() => {
            this.startInFlight = null;
        });
        return await this.startInFlight;
    }

    // The window owns its server. The server is spawned with one inherited
    // socket on fd 3 (--rpc-fd) and stops when that socket closes, which also
    // happens when the extension host exits on a window reload or a crash.
    private async startImpl(): Promise<boolean> {
        if (!this.isIntravenousProject()) {
            return false;
        }
        this.lastTerminalStatusMessage = "";
        this.resetServerReadyState();

        const binary = this.serverBinaryPath();
        if (this.shuttingDown) return false;
        const serverDir = path.dirname(binary);
        const args = [
            "--server",
            "--workspace-root",
            this.workspaceRoot(),
            "--rpc-fd",
            "3",
        ];
        const childEnv: NodeJS.ProcessEnv = {
            ...process.env,
            INTRAVENOUS_DIR: serverDir,
            IV_LOG_FILE: this.serverDiagnosticLogPath,
        };

        this.resetCapturedServerLogs();
        if (process.platform === "linux" && (!childEnv.HOME || !childEnv.XDG_RUNTIME_DIR)) {
            try {
                const { uid, homedir } = os.userInfo();
                childEnv.HOME ||= homedir;
                if (!childEnv.XDG_RUNTIME_DIR) {
                    const runtimeDir = `/run/user/${uid}`;
                    const stat = fs.statSync(runtimeDir, { throwIfNoEntry: false });
                    if (stat?.isDirectory() && stat.uid === uid) {
                        childEnv.XDG_RUNTIME_DIR = runtimeDir;
                        this.outputChannel.appendLine(`Server restored XDG_RUNTIME_DIR: ${runtimeDir}`);
                    }
                }
            } catch (error) {
                this.outputChannel.appendLine(`Server could not restore missing session settings: ${error}`);
            }
        }
        this.outputChannel.appendLine(`Starting server: ${binary}`);
        this.outputChannel.appendLine(`Server cwd: ${this.workspaceRoot()}`);
        this.outputChannel.appendLine(
            `Server diagnostic log: ${this.serverDiagnosticLogPath}`,
        );
        const processLog = fs.openSync(this.serverDiagnosticLogPath, "a");
        let spawnedProcess: childProcess.ChildProcess;
        try {
            spawnedProcess = childProcess.spawn(binary, args, {
                cwd: this.workspaceRoot(),
                env: childEnv,
                stdio: ["ignore", processLog, processLog, "pipe"],
            });
        } finally {
            fs.closeSync(processLog);
        }
        this.process = spawnedProcess;
        this.outputChannel.appendLine(`Startup: server process spawned (PID ${spawnedProcess.pid ?? "unavailable"})`);

        spawnedProcess.on("error", (error) => {
            this.outputChannel.appendLine(`Server spawn failed: ${error.message}`);
        });
        spawnedProcess.on("exit", (code, signal) => {
            if (this.process === spawnedProcess) this.process = null;
            this.rebuildStatusBar.hide();
            this.outputChannel.appendLine(`Server exited: code=${code} signal=${signal}`);
        });

        const socket = spawnedProcess.stdio[3];
        if (!(socket instanceof Duplex)) {
            spawnedProcess.kill();
            throw new Error("Server was spawned without its RPC socket");
        }
        try {
            await this.connectToServer(socket);
            await this.loadInitialSessionState();
        } catch (error) {
            this.logCapturedServerFailureContext();
            if (this.process === spawnedProcess) this.process = null;
            const client = this.client;
            this.client = null;
            this.rpc = null;
            this.connectedAtMs = null;
            client?.dispose();
            spawnedProcess.kill();
            throw error;
        }
        return true;
    }

    private async connectToServer(socket: Duplex): Promise<void> {
        const client = new JsonRpcSocketClient(
            socket,
            (method, params) => {
                if (typeof params.revision === "number") {
                    this.lastSessionRevision = Math.max(
                        this.lastSessionRevision, params.revision);
                }
                void this.notifications.dispatch(method, params);
            },
            (error) => this.handleServerConnectionLost(client, error),
            (revision) => {
                this.lastSessionRevision = revision;
            },
        );
        this.client = client;
        await client.connect();
        if (this.client !== client) throw new Error("Server disconnected before startup completed");
        this.rpc = new WorkspaceRpc(client, () => this.lastSessionRevision);
        // The inherited client is trusted and subscribed to every notification
        // from the moment it is attached, so nothing needs replaying.
        await this.waitForServerReady(this.serverReadyTimeoutMs);
        await this.rpc.hello(`vscode-${process.pid}-${crypto.randomUUID()}`, "");
        this.connectedAtMs = Date.now();
        this.outputChannel.appendLine(
            `Startup: connected at session revision ${this.lastSessionRevision}`,
        );
        // Do not delay graph discovery on hardware enumeration. Report the
        // selected device explicitly because the fallback clock keeps Tick
        // running even when hardware output could not be opened.
        void this.rpc.getAudioDevices().then((snapshot) => {
            if (this.client === client) this.outputChannel.appendLine(audioOutputDiagnostic(snapshot.selectedOutput));
        }, (error) => {
            if (this.client === client) this.outputChannel.appendLine(`Audio output query failed: ${error.message ?? error}`);
        });
    }

    // The server stops with its socket, so a lost connection means this
    // window's server is gone. Unless the window is shutting down, start a new
    // one within the retry budget for this release.
    private handleServerConnectionLost(
        client: JsonRpcSocketClient,
        error: Error,
    ): void {
        if (this.client !== client) return;
        this.manualControls.disconnect();
        this.nodeEvents.disconnected();
        this.resetControlCompilationStatus();
        this.client = null;
        this.rpc = null;
        this.serverReadyReceived = false;
        this.rejectServerReadyWaiters(error);
        this.rebuildStatusBar.hide();
        const lostProcess = this.process;
        this.process = null;
        lostProcess?.kill("SIGTERM");
        if (this.shuttingDown) return;

        const connectedAt = this.connectedAtMs;
        this.connectedAtMs = null;
        // An attempt that never completed its handshake rejects its startup
        // promise. Its caller accounts for that failure.
        if (connectedAt === null) return;
        if (Date.now() - connectedAt >= this.stableServerRunMs) {
            this.restartAttempts = 0;
            this.restartDelayMs = this.initialRestartDelayMs;
        }
        this.setServerStatus("restarting");
        this.outputChannel.appendLine(`Server connection lost: ${error.message}`);
        void this.restartServer();
    }

    private async restartServer(): Promise<void> {
        if (this.restartInFlight) return;
        this.restartInFlight = true;
        try {
            while (!this.shuttingDown && !this.rpc) {
                if (this.restartAttempts >= this.maximumRestartAttempts) {
                    this.stopRestarting("Automatic server retries are exhausted");
                    break;
                }
                await new Promise<void>((resolve) => setTimeout(resolve, this.restartDelayMs));
                if (this.shuttingDown || this.rpc) break;
                ++this.restartAttempts;
                this.restartDelayMs *= 2;
                this.outputChannel.appendLine(`Restarting server (${this.restartAttempts}/${this.maximumRestartAttempts})`);
                try {
                    if (!await this.start()) {
                        if (!this.shuttingDown) this.stopRestarting("The project is no longer available");
                        break;
                    }
                } catch (error) {
                    this.outputChannel.appendLine(
                        `Server restart failed: ${error instanceof Error ? error.message : String(error)}`,
                    );
                }
            }
        } finally {
            this.restartInFlight = false;
        }
        try {
            if (this.rpc && this.lastQuery) {
                const nodes = await this.refreshLastQuery();
                this.updatePrimaryHighlight(nodes);
            }
        } catch (error) {
            this.outputChannel.appendLine(
                `Query after restart failed: ${error instanceof Error ? error.message : String(error)}`,
            );
        }
    }

    private setServerStatus(status: LiveGraphSessionStatus): void {
        this.provider.setSessionStatus(status);
        this.graphSessionStatus = status;
        this.graphChanges.fire();
    }

    private stopRestarting(reason: string): void {
        this.outputChannel.appendLine(`${reason}. Run Intravenous: Retry Server or reload the window.`);
        this.setServerStatus("stopped");
    }

    async retryServer(): Promise<void> {
        if (this.restartInFlight || this.rpc) return;
        this.restartAttempts = 0;
        this.restartDelayMs = this.initialRestartDelayMs;
        this.setServerStatus("starting");
        if (!await this.start() && !this.shuttingDown) this.stopRestarting("The project is no longer available");
    }

    private async loadInitialSessionState(): Promise<void> {
        const rpc = this.rpc;
        if (!rpc) throw new Error("Server disconnected before loading the session");
        const result = await rpc.getNodeInstances();
        if (this.rpc !== rpc) throw new Error("Server disconnected before loading the session");
        this.ivModuleInstances = this.parseIvModuleInstances(result.instances);
        this.projectModuleInstances = this.ivModuleInstances;
        this.refreshVisibleInstances();
        const realizedCount = this.ivModuleInstances.filter(
            (instance) => instance.realized,
        ).length;
        this.outputChannel.appendLine(
            `Instances after ready: total=${this.ivModuleInstances.length} realized=${realizedCount} selected=${this.selectedInstanceId ?? "[none]"}`,
        );
        this.setServerStatus("running");
    }

    private async waitForServerReady(timeoutMs: number): Promise<void> {
        if (this.serverReadyReceived) {
            return;
        }

        await new Promise<void>((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.serverReadyWaiters = this.serverReadyWaiters.filter((waiter) => waiter.timeout !== timeout);
                this.logCapturedServerFailureContext();
                reject(new Error("Server did not report ready before timeout"));
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

    async refreshProjectCatalog(): Promise<void> {
        if (!(await this.ensureReady()) || !this.rpc) return;
        const [packages, instances] = await Promise.all([
            this.rpc.getIvPackageDefinitions(),
            this.rpc.getNodeInstances(),
        ]);
        this.ivPackages = this.parseIvPackageDefinitions(packages.packages);
        this.projectModuleInstances = this.parseIvModuleInstances(instances.instances);
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
        return this.ivModuleInstances.filter((instance) => !isTimelineSnapInstance(instance.instanceId, instance.definitionId));
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
    }

    async saveProject(autosaveOnly = false): Promise<void> {
        if (autosaveOnly && this.graphSessionStatus !== "running") return;
        if (!(await this.ensureReady()) || !this.rpc) {
            return;
        }
        const rpc = this.rpc;
        await this.presentationChanges.catch(() => {});
        await this.manualControls.settled();
        if (rpc === this.rpc) await rpc.saveProject(autosaveOnly);
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
        if (this.shuttingDown || this.graphSessionStatus === "stopped" || this.graphSessionStatus === "restarting")
            return false;
        if (!this.rpc || this.startInFlight) {
            await this.start();
        }
        return this.rpc !== null;
    }

    private resetControlCompilationStatus(): void {
        this.controlCompilationProgress.clear();
        if (this.controlCompilationTimer) clearTimeout(this.controlCompilationTimer);
        this.controlCompilationTimer = undefined;
        this.controlCompilationStatus.hide();
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
        const name = params.packageRoot ? statusBarText(path.basename(params.packageRoot)) : "module";
        this.rebuildStatusBar.text = `$(sync~spin) IV building ${name}`;
        this.rebuildStatusBar.show();
    }

    private showRebuildFailure(): void {
        this.rebuildStatusBar.text = "$(error) IV build failed · See Output";
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
            // The offset is a byte size (fs.statSync), so cut before decoding.
            text = fs.readFileSync(this.serverDiagnosticLogPath)
                .subarray(this.serverDiagnosticLogOffset)
                .toString("utf8");
        } catch {
            return;
        }
        // Everything the server logged since the last report, unabridged and
        // with its indentation: a compiler error or a configuration failure's
        // message precedes its long stack trace.
        const lines = text.split(/\r?\n/).map((line) => line.trimEnd());
        while (lines.length > 0 && lines[0].length === 0) lines.shift();
        while (lines.length > 0 && lines[lines.length - 1].length === 0) lines.pop();
        if (lines.length > 0) {
            this.outputChannel.appendLine("Server failure details:");
            for (const line of lines) this.outputChannel.appendLine(`  ${line}`);
        }
        this.resetCapturedServerLogs();
    }

    private resetServerReadyState(): void {
        this.serverReadyReceived = false;
        this.rejectServerReadyWaiters(new Error("Server startup was restarted"));
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
        this.manualControls.disconnect();
        this.nodeEvents.disconnected();
        this.resetControlCompilationStatus();
        this.controlCompilationStatus.dispose();
        this.manualChanges.dispose();
        this.graphChanges.dispose();
        this.observationChanges.dispose();
        this.shuttingDown = true;
        this.rebuildStatusBar.dispose();
        this.clangdDatabaseWatcher.dispose();
        if (this.clangdRestartTimer) {
            clearTimeout(this.clangdRestartTimer);
            this.clangdRestartTimer = null;
        }
        // Closing the inherited socket stops the server; the signal covers a
        // server that is not reading its socket.
        if (this.client) {
            this.client.dispose();
            this.client = null;
        }
        this.rpc = null;
        if (this.process) {
            this.process.kill("SIGTERM");
            this.process = null;
        }
    }
}

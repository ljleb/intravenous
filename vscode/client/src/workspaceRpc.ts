import { recordedEventTimestampsOperation, type ObservationInterest, type ObservationCapability } from "./observations";
import type { ManualControlId } from "./manualControls";
import { JsonRpcSocketClient, StaleProjectRevisionError } from "./rpcClient";
import { connectionEndMutation, type GraphConnectionEdit } from "./graphConnectionEditing";
import { SymbolicNode, SourceSpan } from "./graphModel";
import { graphScopeWithChildren, laneViewReadBatchSize, type GraphScope, type ProjectEditReceipt } from "./graphScope";
import type { NodeAddress } from "./graphScope";
import { validateNodeEventTarget, type NodeEventProtocol, type SerializedNodeEvent } from "./nodeEvents";
import { NodeEventInteractions } from "./nodeEventInteractions";
import type { PlaybackDrag, PlaybackOutputProfileRequest, PlaybackSnapshot } from "./playback";
import type { NodeDefinitionCatalog, NodeDefinitionSearch } from "./nodeDefinitionSearch";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
};

export type AudioDeviceSelection = {
    deviceId: string | null; name: string | null; available: boolean; error: string | null;
};
export type AudioDevicesSnapshot = {
    outputDevices: Array<{ deviceId: string; name: string }>;
    inputDevices: Array<{ deviceId: string; name: string }>;
    selectedOutput: AudioDeviceSelection; selectedInput: AudioDeviceSelection;
};

export function workspaceNodeEvents(current: () => WorkspaceRpc | null, revision: () => number,
    report: (error: unknown) => void): NodeEventInteractions {
    const requireSession = () => {
        const rpc = current();
        if (!rpc) throw new Error("The server is unavailable");
        return rpc;
    };
    const gesture = async (target: NodeAddress, id: string, begin: boolean) => {
        await requireSession().nodeEventGesture(target, id, begin);
        return revision();
    };
    return new NodeEventInteractions({
        begin: (target, id) => gesture(target, id, true),
        end: (target, id) => gesture(target, id, false),
        send: async (target, event, interactionId) => {
            const rpc = requireSession(), expected = revision();
            const protocol = await rpc.getNodeEventProtocol();
            if (current() !== rpc) throw new Error("The session changed before event dispatch");
            validateNodeEventTarget(protocol, target, event);
            // Standalone edits use the revision before the protocol read.
            // An open gesture uses the server's target and history checks.
            const result = await rpc.dispatchNodeEvent(target, event, expected, interactionId);
            if (!result.ok || result.diagnostics.length)
                throw new Error(result.diagnostics.map((entry) => entry.message).join("\n") || "Node event was not accepted");
            return result.sessionRevision ?? revision();
        },
        report,
    });
}


export class WorkspaceRpc {
    listPresentations(): Promise<{ ready: boolean; entries: unknown[] }> {
        return this.client.request("presentations.list", {});
    }
    fetchPresentation(digest: string): Promise<{ digest: string; bytes: string }> {
        return this.client.request("presentations.fetch", { digest });
    }
    getPlayback(diagnostics = false, outputProfile?: PlaybackOutputProfileRequest): Promise<PlaybackSnapshot> {
        return this.client.request("playback.get", {
            ...(diagnostics ? { diagnostics: true } : {}),
            ...(outputProfile ? { outputProfile } : {}),
        });
    }
    togglePlayback(): Promise<PlaybackSnapshot> { return this.client.request("playback.toggle", {}); }
    setPlaybackRunning(running: boolean): Promise<PlaybackSnapshot> { return this.client.request("playback.setRunning", { running }); }
    seekPlayback(position: string): Promise<PlaybackSnapshot> { return this.client.request("playback.seek", { position }); }
    dragPlayback(position: string, drag: PlaybackDrag): Promise<PlaybackSnapshot> {
        return this.client.request("playback.drag", { position, ...drag });
    }
    constructor(
        private readonly client: JsonRpcSocketClient,
        private readonly currentRevision: () => number = () => 0,
    ) {}

    private mutationParams(
        params: Record<string, unknown>,
        expectedRevision: number = this.currentRevision(),
    ): Record<string, unknown> {
        return {...params, expectedRevision};
    }

    hello(clientId: string, localCapability: string): Promise<{
        protocolVersion: number;
        clientId: string;
        capabilities: string[];
        sessionRevision: number;
    }> {
        return this.client.request("session.hello", { clientId, localCapability });
    }

    subscribe(afterRevision: number, topics: string[] = []): Promise<{
        currentRevision: number;
        oldestAvailableRevision: number;
        snapshotRequired: boolean;
        replayedNotificationCount: number;
        sessionRevision: number;
    }> {
        return this.client.request("session.subscribe", { afterRevision, topics });
    }

    snapshot(): Promise<Record<string, unknown> & { sessionRevision: number }> {
        return this.client.request("session.snapshot", {});
    }

    describeObservations(nodes: NodeAddress[]): Promise<{ capabilities: ObservationCapability[] }> {
        return this.client.request("observations.describe", {
            nodes: nodes.map((node) => ({ ...node, path: node.path ?? [] })),
        });
    }
    reconcileObservations(scopeId: string, interests: ObservationInterest[], predictedInterests?: ObservationInterest[]): Promise<{ revision: string; leaseMs: number }> {
        const normalize = (interest: ObservationInterest) => ({ ...interest, source: { ...interest.source,
            node: { ...interest.source.node, path: interest.source.node.path ?? [] },
        }, ...(interest.source.portKind !== "event" || !interest.window ? {} : {
            window: interest.operation === recordedEventTimestampsOperation ? { begin: interest.window.begin, length: interest.window.length }
                : { ...interest.window, maxMarks: interest.window.points },
        }) });
        return this.client.request("observations.reconcile", {
            scopeId,
            interests: interests.map(normalize),
            ...(predictedInterests?.length ? { predictedInterests: predictedInterests.map(normalize) } : {}),
        });
    }

    getGraphScope(expanded: NodeAddress[] = []): Promise<GraphScope> {
        return this.client.request("graph.getScope", expanded.length ? {
            expanded: expanded.map((address) => ({ ...address, path: address.path ?? [] })),
        } : {});
    }

    private readingLaneViews = false;
    private readonly pendingLaneViewReads = new Map<string, {
        result: Promise<string | null>; resolve: (state: string | null) => void; reject: (error: unknown) => void;
    }>();

    getLaneView(version: string): Promise<string | null> {
        const existing = this.pendingLaneViewReads.get(version);
        if (existing) return existing.result;
        let resolve: (state: string | null) => void;
        let reject: (error: unknown) => void;
        const result = new Promise<string | null>((accept, fail) => { resolve = accept; reject = fail; });
        this.pendingLaneViewReads.set(version, { result, resolve: resolve!, reject: reject! });
        // Let a newly mounted group of lanes enqueue reads before the first RPC.
        if (!this.readingLaneViews) {
            this.readingLaneViews = true;
            queueMicrotask(() => void this.readLaneViewBatches());
        }
        return result;
    }

    private async readLaneViewBatches(): Promise<void> {
        while (this.pendingLaneViewReads.size) {
            const reads = [];
            for (const read of this.pendingLaneViewReads) {
                reads.push(read);
                if (reads.length === laneViewReadBatchSize) break;
            }
            const versions = reads.map(([version]) => version);
            try {
                const { states } = await this.client.request<{ states?: Array<string | null> }>("projectPresentation.getLaneViews", { versions });
                if (!Array.isArray(states) || states.length !== reads.length || states.some((state) => state !== null && typeof state !== "string"))
                    throw new Error("Invalid lane view response");
                reads.forEach(([, read], index) => read.resolve(states[index]));
            } catch (error) {
                for (const [, read] of reads) read.reject(error);
            } finally {
                for (const version of versions) this.pendingLaneViewReads.delete(version);
            }
        }
        this.readingLaneViews = false;
    }

    getNodeEventProtocol(): Promise<NodeEventProtocol> {
        return this.client.request("nodeMessages.getProtocol", {});
    }

    dispatchNodeEvent(target: NodeAddress, event: SerializedNodeEvent, expectedRevision: number, interactionId?: string): Promise<{
        ok: boolean; diagnostics: Array<{ target: NodeAddress; message: string }>; sessionRevision?: number;
    }> {
        return this.client.request("nodeMessages.dispatch", { target: { ...target, path: target.path ?? [] },
            eventTypeNominalId: event.eventType.nominalId,
            eventTypeDefinitionFingerprint: event.eventType.definitionFingerprint,
            payload: event.payload, ...(interactionId ? { interactionId } : { expectedRevision }) });
    }

    nodeEventGesture(target: NodeAddress, interactionId: string, begin: boolean): Promise<void> {
        const params = { target: { ...target, path: target.path ?? [] }, interactionId };
        return this.client.request(`nodeMessages.${begin ? "beginGesture" : "endGesture"}`,
            begin ? this.mutationParams(params) : params);
    }

    getAudioDevices(): Promise<AudioDevicesSnapshot> {
        return this.client.request("audioDevices.get", {});
    }

    getNodeInstances(sourceFilePath: string | null = null): Promise<{ instances?: Array<Record<string, unknown>>; sessionRevision?: number }> {
        return this.client.request("nodeInstances.get", sourceFilePath ? { sourceFilePath } : {});
    }

    createNodeInstance(
        definitionId: string,
        displayName: string | null = null,
        configurationSource = "",
        instanceId: string | null = null,
    ): Promise<{ instanceId: string }> {
        const params: Record<string, unknown> = { definitionId };
        if (instanceId != null) {
            params.instanceId = instanceId;
        }
        if (displayName != null) {
            params.displayName = displayName;
        }
        if (configurationSource.length > 0) {
            params.configurationSource = configurationSource;
        }
        return this.client.request(
            "nodeInstances.create", this.mutationParams(params));
    }

    deleteNodeInstance(instanceId: string): Promise<void> {
        return this.client.request(
            "nodeInstances.delete",
            this.mutationParams({ instanceId }),
        );
    }

    updateNodeInstances(
        updates: Array<{
            instanceId: string;
            displayName?: string | null;
            configurationSource?: string | null;
        }>,
        expectedRevision?: number,
    ): Promise<void> {
        return this.client.request(
            "nodeInstances.update", this.mutationParams({ updates }, expectedRevision));
    }

    upsertGraphConnection(connection: Record<string, unknown>, expectedRevision?: number): Promise<void> {
        return this.client.request(
            "graphConnections.upsert", this.mutationParams({ connection }, expectedRevision));
    }

    deleteGraphConnection(connectionId: string, expectedRevision?: number): Promise<void> {
        return this.client.request(
            "graphConnections.delete", this.mutationParams({ connectionId }, expectedRevision));
    }

    async editGraphConnectionEnd(edit: GraphConnectionEdit, expanded: NodeAddress[]): Promise<void> {
        const attempts = 4;
        for (let attempt = 0; ; ++attempt) {
            const beforeRead = this.currentRevision();
            const scope = graphScopeWithChildren(await this.getGraphScope(expanded));
            const mutation = connectionEndMutation(scope, edit);
            const revision = scope.sessionRevision ?? beforeRead;
            try {
                if (mutation?.type === "delete") await this.deleteGraphConnection(mutation.id, revision);
                else if (mutation?.type === "upsert") await this.upsertGraphConnection(mutation.connection, revision);
                return;
            } catch (error) {
                if (!(error instanceof StaleProjectRevisionError) || attempt + 1 >= attempts) throw error;
                // The server rejected the write before applying it. Recheck the
                // captured connection against a fresh scope before trying again.
            }
        }
    }

    applyProjectEdit(operations: Array<{ method: string; params: Record<string, unknown> }>, expectedRevision?: number | null): Promise<ProjectEditReceipt> {
        return this.client.request("project.applyEdit", expectedRevision === null ? { operations }
            : this.mutationParams({ operations }, expectedRevision));
    }

    projectHistory(direction: "undo" | "redo"): Promise<void> {
        return this.client.request(`project.${direction}`, this.mutationParams({}));
    }

    hoverManualControl(control: ManualControlId | null): Promise<void> {
        return this.client.request("manualControls.hover", control ? { ...control } : {});
    }
    toggleManualInput(control: ManualControlId): Promise<void> {
        return this.client.request("manualControls.toggleInput", { ...control });
    }
    beginManualControl(interactionId: string, control: ManualControlId, value: number): Promise<void> {
        return this.client.request("manualControls.begin", { interactionId, ...control, value });
    }
    // A live gesture value: unacknowledged, revision-free, latest value wins.
    updateManualControl(interactionId: string, value: number): void {
        this.client.notify("manualControls.update", { interactionId, value });
    }
    endManualControl(interactionId: string, value: number): Promise<void> {
        return this.client.request("manualControls.end", { interactionId, value });
    }

    getIvPackageDefinitions(): Promise<{ packages?: Array<Record<string, unknown>> }> {
        return this.client.request("ivPackages.list", {});
    }

    nodeDefinitionCatalog(): Promise<NodeDefinitionCatalog> {
        return this.client.request("nodeDefinitions.search", { source: "" });
    }

    searchNodeDefinitions(source: string, cursor: number): Promise<NodeDefinitionSearch> {
        return this.client.request("nodeDefinitions.search", { source, cursor });
    }

    createIvPackage(name: string): Promise<{ package: Record<string, unknown> }> {
        return this.client.request(
            "ivPackages.create", this.mutationParams({ name }));
    }

    shutdown(): Promise<void> {
        return this.client.request("server.shutdown", {});
    }

    saveProject(autosaveOnly = false): Promise<void> {
        return this.client.request("project.save", autosaveOnly ? { autosaveOnly } : {});
    }

    enableProjectAutosave(): Promise<void> {
        return this.client.request(
            "project.enableAutosave", this.mutationParams({}));
    }

    disableProjectAutosave(): Promise<void> {
        return this.client.request(
            "project.disableAutosave", this.mutationParams({}));
    }

    queryNodesBySpans(
        filePath: string,
        ranges: SourceQueryRange[],
        match: "union" | "intersection",
        instanceId: string | null,
    ): Promise<{ nodes?: SymbolicNode[] }> {
        const params: Record<string, unknown> = { filePath, ranges, match };
        if (instanceId != null) {
            params.instanceId = instanceId;
        }
        return this.client.request("graph.queryBySpans", params);
    }

    queryActiveRegions(filePath: string): Promise<{ sourceSpans?: SourceSpan[] }> {
        return this.client.request("graph.queryActiveRegions", { filePath });
    }





}

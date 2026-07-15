import { JsonRpcSocketClient } from "./rpcClient";
import { LogicalNode, SourceSpan } from "./graphModel";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
};

type LaneViewParams = {
    viewId: string;
    filter: { query?: string; kind?: string };
    startIndex: number;
    visibleLaneCount: number;
    firstSampleIndex?: number;
    lastSampleIndex?: number;
    displaySampleCount?: number;
};

export class WorkspaceRpc {
    constructor(private readonly client: JsonRpcSocketClient) {}

    getIvModuleInstances(sourceFilePath: string | null = null): Promise<{ instances?: Array<Record<string, unknown>> }> {
        return this.client.request("ivModuleInstances.get", sourceFilePath ? { sourceFilePath } : {});
    }

    createIvModuleInstance(moduleId: string, displayName: string | null = null): Promise<{ instanceId: string }> {
        const params: Record<string, unknown> = { moduleId };
        if (displayName != null) {
            params.displayName = displayName;
        }
        return this.client.request("ivModuleInstances.create", params);
    }

    deleteIvModuleInstance(instanceId: string): Promise<void> {
        return this.client.request("ivModuleInstances.delete", { instanceId });
    }

    updateIvModuleInstances(
        updates: Array<{ instanceId: string; displayName?: string | null; defaultSilenceTtlSamples?: number | null }>,
    ): Promise<void> {
        return this.client.request("ivModuleInstances.update", { updates });
    }

    getIvModuleSources(): Promise<{ sources?: Array<Record<string, unknown>> }> {
        return this.client.request("ivModuleSources.list", {});
    }

    createIvModuleSource(name: string): Promise<{ source: Record<string, unknown> }> {
        return this.client.request("ivModuleSources.create", { name });
    }

    shutdown(): Promise<void> {
        return this.client.request("server.shutdown", {});
    }

    pausePlayback(): Promise<void> {
        return this.client.request("playback.pause", {});
    }

    resumePlayback(startIndex: number): Promise<void> {
        return this.client.request("playback.resume", {
            startIndex,
        });
    }

    seekPlayback(sampleIndex: number): Promise<void> {
        return this.client.request("playback.seek", { sampleIndex });
    }

    saveProject(): Promise<void> {
        return this.client.request("project.save", {});
    }

    enableProjectAutosave(): Promise<void> {
        return this.client.request("project.enableAutosave", {});
    }

    disableProjectAutosave(): Promise<void> {
        return this.client.request("project.disableAutosave", {});
    }

    queryNodesBySpans(
        filePath: string,
        ranges: SourceQueryRange[],
        match: "union" | "intersection",
        instanceId: string | null,
    ): Promise<{ nodes?: LogicalNode[] }> {
        const params: Record<string, unknown> = { filePath, ranges, match };
        if (instanceId != null) {
            params.instanceId = instanceId;
        }
        return this.client.request("graph.queryBySpans", params);
    }

    queryActiveRegions(filePath: string): Promise<{ sourceSpans?: SourceSpan[] }> {
        return this.client.request("graph.queryActiveRegions", { filePath });
    }

    setSampleInputValue(nodeId: string, inputOrdinal: number, value: unknown, memberOrdinal: number | null): Promise<void> {
        const params: Record<string, unknown> = { nodeId, inputOrdinal, value };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        return this.client.request("graph.setSampleInputValue", params);
    }

    setSampleInputState(nodeId: string, inputOrdinal: number, state: string, memberOrdinal: number | null): Promise<void> {
        const params: Record<string, unknown> = { nodeId, inputOrdinal, state };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        return this.client.request("graph.setSampleInputState", params);
    }

    setEventInputState(nodeId: string, inputOrdinal: number, state: string, memberOrdinal: number | null): Promise<void> {
        const params: Record<string, unknown> = { nodeId, inputOrdinal, state };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        return this.client.request("graph.setEventInputState", params);
    }

    setSampleOutputState(nodeId: string, outputOrdinal: number, state: string, memberOrdinal: number | null): Promise<void> {
        const params: Record<string, unknown> = { nodeId, outputOrdinal, state };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        return this.client.request("graph.setSampleOutputState", params);
    }

    setEventOutputState(nodeId: string, outputOrdinal: number, state: string, memberOrdinal: number | null): Promise<void> {
        const params: Record<string, unknown> = { nodeId, outputOrdinal, state };
        if (memberOrdinal != null) {
            params.memberOrdinal = memberOrdinal;
        }
        return this.client.request("graph.setEventOutputState", params);
    }

    setTimelineLaneUiState(
        laneId: string,
        serializedState: string,
        expectedRevision?: number,
    ): Promise<void> {
        const params: Record<string, unknown> = { laneId, serializedState };
        if (expectedRevision != null) params.expectedRevision = expectedRevision;
        return this.client.request("timeline.setLaneUiState", params);
    }

    setTimelineLaneName(laneId: string, name: string): Promise<void> {
        return this.client.request("timeline.setLaneUiState", { laneId, name });
    }

    createTimelineLane(typeId: string): Promise<void> {
        return this.client.request("timeline.createLane", { typeId });
    }

    connectTimelineLanes(
        sourceLaneId: string,
        targetLaneId: string,
        portDomain: "realtime" | "compiled",
        portKind: "sample" | "event",
        portOrdinal: number,
    ): Promise<void> {
        return this.client.request("timeline.connectLanes", {
            sourceLaneId,
            targetLaneId,
            portDomain,
            portKind,
            portOrdinal,
        });
    }

    disconnectTimelineLanes(
        sourceLaneId: string,
        targetLaneId: string,
        portDomain: "realtime" | "compiled",
        portKind: "sample" | "event",
        portOrdinal: number,
    ): Promise<void> {
        return this.client.request("timeline.disconnectLanes", {
            sourceLaneId, targetLaneId, portDomain, portKind, portOrdinal,
        });
    }

    getTimelineLaneTypes(): Promise<{ laneTypes?: Array<{
        typeId: string; category: string; label: string; description: string;
    }> }> {
        return this.client.request("timeline.laneTypes", {});
    }

    openLaneView(params: LaneViewParams): Promise<Record<string, unknown>> {
        return this.client.request("timeline.openLaneView", params);
    }

    updateLaneView(params: LaneViewParams): Promise<Record<string, unknown>> {
        return this.client.request("timeline.updateLaneView", params);
    }

    closeLaneView(viewId: string): Promise<void> {
        return this.client.request("timeline.closeLaneView", { viewId });
    }
}

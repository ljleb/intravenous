import { JsonRpcSocketClient } from "./rpcClient";
import { LogicalNode, SourceSpan } from "./graphModel";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
};

type LaneViewParams = {
    viewId: string;
    filter: { kind: string };
    startIndex: number;
    visibleLaneCount: number;
};

export class WorkspaceRpc {
    constructor(private readonly client: JsonRpcSocketClient) {}

    getIvModuleInstances(sourceFilePath: string | null = null): Promise<{ instances?: Array<Record<string, unknown>> }> {
        return this.client.request("ivModuleInstances.get", sourceFilePath ? { sourceFilePath } : {});
    }

    createIvModuleInstance(moduleId: string): Promise<{ instanceId: string }> {
        return this.client.request("ivModuleInstances.create", { moduleId });
    }

    deleteIvModuleInstance(instanceId: string): Promise<void> {
        return this.client.request("ivModuleInstances.delete", { instanceId });
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

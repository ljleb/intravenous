import type { ManualControlId } from "./manualControls";
import { JsonRpcSocketClient } from "./rpcClient";
import { VirtualNode, SourceSpan } from "./graphModel";
import type { GraphScope } from "./graphScope";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
};


export class WorkspaceRpc {
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

    snapshot(): Promise<Record<string, unknown> & { sessionRevision?: number }> {
        return this.client.request("session.snapshot", {});
    }

    getGraphScope(): Promise<GraphScope> {
        return this.client.request("graph.getScope", {});
    }

    getNodeInstances(sourceFilePath: string | null = null): Promise<{ instances?: Array<Record<string, unknown>> }> {
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
    ): Promise<void> {
        return this.client.request(
            "nodeInstances.update", this.mutationParams({ updates }));
    }

    upsertGraphConnection(connection: Record<string, unknown>, expectedRevision?: number): Promise<void> {
        return this.client.request(
            "graphConnections.upsert", this.mutationParams({ connection }, expectedRevision));
    }

    deleteGraphConnection(connectionId: string, expectedRevision?: number): Promise<void> {
        return this.client.request(
            "graphConnections.delete", this.mutationParams({ connectionId }, expectedRevision));
    }

    applyProjectEdit(operations: Array<{ method: string; params: Record<string, unknown> }>, expectedRevision?: number): Promise<void> {
        return this.client.request("project.applyEdit", this.mutationParams({ operations }, expectedRevision));
    }

    projectHistory(direction: "undo" | "redo"): Promise<void> {
        return this.client.request(`project.${direction}`, this.mutationParams({}));
    }

    hoverManualControl(control: ManualControlId | null): Promise<void> {
        return this.client.request("manualControls.hover", control ? { ...control } : {});
    }
    beginManualControl(interactionId: string, control: ManualControlId, value: number): Promise<void> {
        return this.client.request("manualControls.begin", this.mutationParams({ interactionId, ...control, value }));
    }
    updateManualControl(interactionId: string, value: number): Promise<void> {
        return this.client.request("manualControls.update", this.mutationParams({ interactionId, value }));
    }
    endManualControl(interactionId: string, value: number): Promise<void> {
        return this.client.request("manualControls.end", this.mutationParams({ interactionId, value }));
    }

    getIvPackageDefinitions(): Promise<{ packages?: Array<Record<string, unknown>> }> {
        return this.client.request("ivPackages.list", {});
    }

    createIvPackage(name: string): Promise<{ package: Record<string, unknown> }> {
        return this.client.request(
            "ivPackages.create", this.mutationParams({ name }));
    }

    shutdown(): Promise<void> {
        return this.client.request("server.shutdown", {});
    }

    saveProject(): Promise<void> {
        return this.client.request("project.save", {});
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
    ): Promise<{ nodes?: VirtualNode[] }> {
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

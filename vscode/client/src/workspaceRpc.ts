import { JsonRpcSocketClient } from "./rpcClient";
import { VirtualNode, SourceSpan } from "./graphModel";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
};


export class WorkspaceRpc {
    constructor(private readonly client: JsonRpcSocketClient) {}

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
        return this.client.request("nodeInstances.create", params);
    }

    deleteNodeInstance(instanceId: string): Promise<void> {
        return this.client.request("nodeInstances.delete", { instanceId });
    }

    updateNodeInstances(
        updates: Array<{
            instanceId: string;
            displayName?: string | null;
            configurationSource?: string | null;
        }>,
    ): Promise<void> {
        return this.client.request("nodeInstances.update", { updates });
    }

    upsertGraphConnection(connection: Record<string, unknown>): Promise<void> {
        return this.client.request("graphConnections.upsert", { connection });
    }

    deleteGraphConnection(connectionId: string): Promise<void> {
        return this.client.request("graphConnections.delete", { connectionId });
    }

    getIvPackageDefinitions(): Promise<{ packages?: Array<Record<string, unknown>> }> {
        return this.client.request("ivPackages.list", {});
    }

    createIvPackage(name: string): Promise<{ package: Record<string, unknown> }> {
        return this.client.request("ivPackages.create", { name });
    }

    shutdown(): Promise<void> {
        return this.client.request("server.shutdown", {});
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

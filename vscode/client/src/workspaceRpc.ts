import { JsonRpcSocketClient } from "./rpcClient";
import { VirtualNode, SourceSpan } from "./graphModel";

export type SourceQueryRange = {
    start: { line: number; column: number };
    end: { line: number; column: number };
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
        updates: Array<{ instanceId: string; displayName?: string | null }>,
    ): Promise<void> {
        return this.client.request("ivModuleInstances.update", { updates });
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

import { VirtualNode } from "./graphModel";
import { LiveGraphInstance } from "./liveGraphProtocol";

// The node-instance fields the live graph view reads from nodeInstances.get
// and nodeInstances.updated. Every field is optional on the wire.
export type LiveGraphInstanceRecord = {
    instanceId?: string;
    definitionId?: string;
    displayName?: string;
    moduleId?: string;
    packageRoot?: string;
    realized?: boolean;
};

export function liveGraphInstances(records: LiveGraphInstanceRecord[]): LiveGraphInstance[] {
    return records.flatMap((record) => {
        if (typeof record.instanceId !== "string" || record.instanceId.length === 0) {
            return [];
        }
        return [{
            instanceId: record.instanceId,
            definitionId: record.definitionId ?? "",
            displayName: record.displayName ?? "",
            moduleId: record.moduleId ?? "",
            packageRoot: record.packageRoot ?? "",
            realized: record.realized === true,
        }];
    });
}

// Applies a graph.nodesUpdated delta: every node of a replaced instance is
// dropped, then each updated node replaces the node with the same ID.
export function upsertVirtualNodes(
    current: VirtualNode[],
    updates: VirtualNode[],
    replaceInstanceIds: string[],
): VirtualNode[] {
    const replaced = new Set(replaceInstanceIds);
    const byId = new Map<string, VirtualNode>();
    for (const node of current) {
        if (node.instanceId && replaced.has(node.instanceId)) {
            continue;
        }
        byId.set(node.id ?? "", node);
    }
    for (const node of updates) {
        if (node.id) {
            byId.set(node.id, node);
        }
    }
    return [...byId.values()];
}

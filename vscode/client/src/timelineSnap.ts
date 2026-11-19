import { beatBpm, readBeatGrid } from "./beatGrid";
import type { GraphPortMatcher, GraphScope, GraphScopeConnection, GraphScopeNode, NodeAddress } from "./graphScope";

export type TimelineGridMode = "seconds" | "beats" | "custom";
export type ProjectEditOperation = { method: string; params: Record<string, unknown> };
type SnapConnection = GraphScopeConnection["connection"] & { connectionId: string };
export type TimelineSnapMemory = { beats?: [number, number, number[]]; custom?: SnapConnection[] };

// Stable identities let the graph specify the mode after undo, redo, and reload.
// Authored instances of either definition keep their own presentation.
const beatsInstanceId = "c17d95d0-80f9-4eec-a184-a2be63cc6d04";
const timelineInstanceId = "9962e717-db59-46a8-bb00-1876f1935504";
const generatedConnectionId = "25723a79-67d9-4fcc-9d36-a3ffe6365bdc";
const beatsDefinition = "iv.builtin.beats";
const timelineDefinition = "iv.builtin.timeline";

function hasRootAddress(address: NodeAddress, instanceId: string): boolean {
    return address.instanceId === instanceId && !address.path?.length;
}
export function isTimelineSnapAddress(address: NodeAddress): boolean {
    return hasRootAddress(address, timelineInstanceId);
}
export function isTimelineSnapNode(node: GraphScopeNode): boolean {
    return !node.address.path?.length && isTimelineSnapInstance(node.address.instanceId, node.instance.definitionId);
}
export function isTimelineSnapInstance(instanceId?: string, definitionId?: string): boolean {
    return (instanceId === timelineInstanceId && definitionId === timelineDefinition)
        || (instanceId === beatsInstanceId && definitionId === beatsDefinition);
}
export function managedTimeline(scope: GraphScope): GraphScopeNode | undefined {
    return scope.nodes.find((node) => isTimelineSnapAddress(node.address) && node.instance.definitionId === timelineDefinition);
}
export function managedBeats(scope: GraphScope): GraphScopeNode | undefined {
    return scope.nodes.find((node) => hasRootAddress(node.address, beatsInstanceId) && node.instance.definitionId === beatsDefinition);
}
export function timelineGridMode(scope: GraphScope): TimelineGridMode {
    return !managedTimeline(scope) ? "seconds" : managedBeats(scope) ? "beats" : "custom";
}
export function isGeneratedSnapConnection(entry: GraphScopeConnection): boolean {
    return entry.id === generatedConnectionId && entry.connection.kind === "event"
        && entry.connection.outputs.length === 1 && hasRootAddress(entry.connection.outputs[0], beatsInstanceId)
        && entry.connection.inputs.length === 1 && isTimelineSnapAddress(entry.connection.inputs[0]);
}

export function readTimelineSnapMemory(value: unknown): TimelineSnapMemory {
    if (value === undefined) return {};
    if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("Invalid timeline snapping settings");
    const record = value as Partial<TimelineSnapMemory>;
    const memory: TimelineSnapMemory = {};
    if (record.beats !== undefined) {
        const grid = Array.isArray(record.beats) ? readBeatGrid(record.beats) : null;
        if (!grid) throw new Error("Invalid saved Beats settings");
        memory.beats = [beatBpm(grid), grid.beatsPerBar, [...grid.subdivisions]];
    }
    if (record.custom !== undefined) {
        const matcher = (entry: GraphPortMatcher) => !!entry && typeof entry.instanceId === "string"
            && (entry.path === undefined || Array.isArray(entry.path)) && !!entry.port && typeof entry.port === "object";
        if (!Array.isArray(record.custom) || record.custom.some((entry) => !entry || entry.kind !== "event"
            || typeof entry.connectionId !== "string" || entry.connectionId === generatedConnectionId
            || typeof entry.sourceType !== "string" || entry.targetType !== "beat"
            || !Array.isArray(entry.outputs) || !entry.outputs.length || !entry.outputs.every(matcher)
            || !Array.isArray(entry.inputs) || !entry.inputs.length
            || !entry.inputs.every((input) => matcher(input) && isTimelineSnapAddress(input)))) {
            throw new Error("Invalid saved Custom snap connections");
        }
        memory.custom = record.custom;
    }
    return memory;
}

export function planTimelineSnapMode(scope: GraphScope, mode: TimelineGridMode, memory: TimelineSnapMemory,
    newConnectionId: () => string): { memory: TimelineSnapMemory; operations: ProjectEditOperation[] } {
    if (!scope.editable) throw new Error("This graph is read-only");
    const timeline = managedTimeline(scope);
    const beats = managedBeats(scope);
    const generated = scope.connections.find((entry) => entry.id === generatedConnectionId);
    if (scope.nodes.some((node) => (isTimelineSnapAddress(node.address) && node !== timeline)
        || (hasRootAddress(node.address, beatsInstanceId) && node !== beats))
        || generated && !isGeneratedSnapConnection(generated)) {
        throw new Error("An editor-managed snapping identity is already in use");
    }
    if (mode === timelineGridMode(scope)) return { memory, operations: [] };
    const remembered = { ...memory };
    const operations: ProjectEditOperation[] = [];
    const upsert = (connection: SnapConnection) => operations.push({ method: "graphConnections.upsert", params: { connection } });
    const deleteConnection = (connectionId: string) => operations.push({ method: "graphConnections.delete", params: { connectionId } });
    const deleteNode = (instanceId: string) => operations.push({ method: "nodeInstances.delete", params: { instanceId } });

    if (timeline && !beats) {
        const custom: SnapConnection[] = [];
        for (const { id, connection } of scope.connections) {
            if (id === generatedConnectionId || connection.kind !== "event") continue;
            const inputs = connection.inputs.filter(isTimelineSnapAddress);
            if (!inputs.length) continue;
            const otherInputs = connection.inputs.filter((input) => !isTimelineSnapAddress(input));
            // Preserve authored legs of a fanout while the ruler leg is dormant.
            if (otherInputs.length) upsert({ ...connection, connectionId: id, inputs: otherInputs });
            else deleteConnection(id);
            custom.push({ ...connection, connectionId: otherInputs.length ? newConnectionId() : id, inputs });
        }
        remembered.custom = custom;
    }
    if (beats && mode !== "beats") {
        const grid = beats.construction?.definitionId === beatsDefinition ? readBeatGrid(beats.construction.arguments) : null;
        if (grid) remembered.beats = [beatBpm(grid), grid.beatsPerBar, [...grid.subdivisions]];
        deleteNode(beatsInstanceId);
    }
    if (generated && mode !== "beats") deleteConnection(generatedConnectionId);
    if (timeline && mode === "seconds") deleteNode(timelineInstanceId);
    if (!timeline && mode !== "seconds") {
        operations.push({ method: "nodeInstances.create", params: { instanceId: timelineInstanceId, definitionId: timelineDefinition } });
    }
    if (mode === "beats") {
        if (!beats) {
            const args = remembered.beats;
            const configurationSource = args ? args[0] + ", " + args[1] + ", {" + args[2].join(", ") + "}" : "";
            operations.push({ method: "nodeInstances.create", params: { instanceId: beatsInstanceId,
                definitionId: beatsDefinition, configurationSource } });
        }
        if (!generated) {
            const port = { name: "beats", index: 0 };
            upsert({ connectionId: generatedConnectionId, kind: "event", sourceType: "beat", targetType: "beat",
                outputs: [{ instanceId: beatsInstanceId, path: [], port }], inputs: [{ instanceId: timelineInstanceId, path: [], port }] });
        }
    } else if (mode === "custom") {
        const ids = new Set(scope.connections.map(({ id }) => id));
        const instances = new Set(scope.nodes.map(({ address }) => address.instanceId));
        for (const connection of memory.custom ?? []) {
            const outputs = connection.outputs.filter(({ instanceId }) => instances.has(instanceId));
            if (!outputs.length) continue;
            upsert({ ...connection, outputs, connectionId: ids.has(connection.connectionId) ? newConnectionId() : connection.connectionId });
        }
    }
    return { memory: remembered, operations };
}

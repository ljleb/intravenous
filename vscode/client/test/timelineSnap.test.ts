import test from "node:test";
import assert from "node:assert/strict";
import { connectionFromPorts, type GraphPortEndpoint } from "../src/graphConnectionEditing";
import type { GraphScope, GraphScopeConnection, GraphScopeNode } from "../src/graphScope";
import { isGeneratedSnapConnection, isTimelineSnapNode, managedBeats, managedTimeline, planTimelineSnapMode,
    readTimelineSnapMemory, timelineGridMode, type ProjectEditOperation } from "../src/timelineSnap";

function node(instanceId: string, definitionId: string): GraphScopeNode {
    return {
        address: { instanceId }, loadState: "ready", instance: { instanceId, definitionId, realized: true },
        sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [], hasChildren: false,
    };
}

function apply(scope: GraphScope, operations: ProjectEditOperation[]): GraphScope {
    const nodes = new Map(scope.nodes.map((entry) => [entry.address.instanceId, entry]));
    const connections = new Map(scope.connections.map((entry) => [entry.id, entry]));
    for (const { method, params } of operations) {
        if (method === "nodeInstances.create") {
            const instanceId = String(params.instanceId), definitionId = String(params.definitionId);
            const entry = node(instanceId, definitionId);
            if (definitionId === "builtin.timeline") entry.eventInputs = [{ name: "beats", index: 0, eventType: "beat" }];
            nodes.set(instanceId, entry);
        } else if (method === "nodeInstances.delete") nodes.delete(String(params.instanceId));
        else if (method === "graphConnections.delete") connections.delete(String(params.connectionId));
        else if (method === "graphConnections.upsert") {
            const connection = params.connection as GraphScopeConnection["connection"];
            const id = connection.connectionId!;
            connections.set(id, { id, applied: true, connection });
        } else assert.fail(method);
    }
    return { ...scope, nodes: [...nodes.values()], connections: [...connections.values()] };
}

function empty(): GraphScope {
    return { editable: true, generation: 1, nodes: [], connections: [] };
}
const unusedId = () => assert.fail("unexpected replacement connection id");

test("the graph owns the snap mode and retains the accepted Beats settings across removal", () => {
    const authored = node("authored-beats", "builtin.beats");
    const scope = { ...empty(), nodes: [authored] };
    const created = planTimelineSnapMode(scope, "beats", {}, unusedId);
    const beats = apply(scope, created.operations);
    assert.equal(timelineGridMode(beats), "beats");
    assert.deepEqual(beats.nodes.filter((entry) => !isTimelineSnapNode(entry)), [authored]);
    assert.equal(beats.connections.filter(isGeneratedSnapConnection).length, 1);
    assert.deepEqual(planTimelineSnapMode(beats, "beats", {}, unusedId).operations, []);

    managedBeats(beats)!.construction = { definitionId: "builtin.beats", arguments: [137.5, 7, [3, 2]] };
    const removed = planTimelineSnapMode(beats, "seconds", {}, unusedId);
    assert.deepEqual(removed.memory.beats, [137.5, 7, [3, 2]]);
    const seconds = apply(beats, removed.operations);
    assert.deepEqual(seconds, scope);
    const restored = planTimelineSnapMode(seconds, "beats", readTimelineSnapMemory(removed.memory), unusedId);
    assert.equal(restored.operations.find(({ params }) => params.definitionId === "builtin.beats")?.params.configurationSource,
        "137.5, 7, {3, 2}");
});

test("Custom remembers multiple sources and preserves the authored part of a shared connection", () => {
    const initial = { ...empty(), nodes: [node("one", "example.source"), node("two", "example.source"), node("other", "builtin.timeline")] };
    const custom = apply(initial, planTimelineSnapMode(initial, "custom", {}, unusedId).operations);
    const timeline = managedTimeline(custom)!;
    custom.connections = ["one", "two"].map((id) => ({ id, applied: true, connection: {
        connectionId: id, kind: "event", sourceType: "beat", targetType: "beat",
        outputs: [{ instanceId: id, port: { name: "out", index: 0 } }],
        inputs: [{ ...timeline.address, port: { name: "beats", index: 0 } },
            ...(id === "one" ? [{ instanceId: "other", port: { name: "beats" } }] : [])],
    } }));
    const toBeats = planTimelineSnapMode(custom, "beats", {}, () => "detached-snap-leg");
    const beats = apply(custom, toBeats.operations);
    assert.equal(timelineGridMode(beats), "beats");
    assert.deepEqual(managedTimeline(beats)?.address, timeline.address);
    assert.deepEqual(beats.connections.find(({ id }) => id === "one")?.connection.inputs, [{ instanceId: "other", port: { name: "beats" } }]);
    const back = planTimelineSnapMode(beats, "custom", readTimelineSnapMemory(toBeats.memory), unusedId);
    const restored = apply(beats, back.operations);
    assert.equal(timelineGridMode(restored), "custom");
    assert.deepEqual(restored.connections.filter(({ connection }) => connection.inputs.some((input) => input.instanceId === timeline.address.instanceId))
        .map(({ connection }) => connection.outputs[0].instanceId).sort(), ["one", "two"]);
    assert.ok(!restored.connections.some(isGeneratedSnapConnection));

    const afterSourceDeletion = { ...beats, nodes: beats.nodes.filter(({ address }) => address.instanceId !== "two") };
    const remaining = planTimelineSnapMode(afterSourceDeletion, "custom", toBeats.memory, unusedId);
    assert.deepEqual(apply(afterSourceDeletion, remaining.operations).connections
        .filter(({ connection }) => connection.inputs.some((input) => input.instanceId === timeline.address.instanceId))
        .map(({ connection }) => connection.outputs[0].instanceId), ["one"]);
});

test("the Custom ruler socket accepts Beat Event outputs only", () => {
    const source = node("source", "example.source");
    source.eventOutputs = [{ name: "beat", index: 0, eventType: "beat" }, { name: "another", index: 1, eventType: "midi" }];
    const initial = { ...empty(), nodes: [source] };
    const custom = apply(initial, planTimelineSnapMode(initial, "custom", {}, unusedId).operations);
    const input: GraphPortEndpoint = { address: managedTimeline(custom)!.address, kind: "event", direction: "input", name: "beats", index: 0 };
    const output: GraphPortEndpoint = { address: source.address, kind: "event", direction: "output", name: "beat", index: 0 };
    assert.equal(connectionFromPorts(custom, "wire", output, input).targetType, "beat");
    assert.throws(() => connectionFromPorts(custom, "wire", { ...output, name: "another", index: 1 }, input), /Beat Event/);
    const beats = apply(custom, planTimelineSnapMode(custom, "beats", {}, unusedId).operations);
    assert.throws(() => connectionFromPorts(beats, "wire", output, input), /Custom/);
});

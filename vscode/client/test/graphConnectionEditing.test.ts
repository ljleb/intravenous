import test from "node:test";
import assert from "node:assert/strict";
import { canMoveConnectionEnd, connectionEndMutation, connectionEndsAtPort, connectionFromPorts, fixedConnectionPorts,
    type GraphConnectionEnd, type GraphPortEndpoint } from "../src/graphConnectionEditing";
import type { GraphPortMatcher, GraphScope, GraphScopeConnection, GraphScopeNode } from "../src/graphScope";
import { beginPortConnection, canDropPortConnection, finishPortConnection, movePortConnection } from "../src/portConnectionGesture";

function scope(): GraphScope {
    const node = (id: string): GraphScopeNode => ({
        address: { instanceId: id }, instance: { instanceId: id, definitionId: "test.node", realized: true },
        loadState: "ready", hasChildren: false,
        sampleInputs: [{ name: "in", index: 0, channelType: "mono" }],
        sampleOutputs: [{ name: "out", index: 0, channelType: "stereo" }],
        eventInputs: [{ name: "gate", index: 0, eventType: "trigger" }],
        eventOutputs: [{ name: "notes", index: 0, eventType: "midi" }],
    });
    return { generation: 1, editable: true, nodes: [node("a"), node("b")], connections: [] };
}
const output: GraphPortEndpoint = { address: { instanceId: "a" }, kind: "sample", direction: "output", index: 0, name: "out" };
const input: GraphPortEndpoint = { address: { instanceId: "b" }, kind: "sample", direction: "input", index: 0, name: "in" };

test("connection drag works in either direction and preserves channel conversion", () => {
    const connection = connectionFromPorts(scope(), "wire", output, input);
    assert.deepEqual(connectionFromPorts(scope(), "wire", input, output), connection);
    assert.deepEqual(connection, { connectionId: "wire", kind: "sample", sourceType: "stereo", targetType: "mono",
        outputs: [{ instanceId: "a", path: [], port: { name: "out", index: 0 } }],
        inputs: [{ instanceId: "b", path: [], port: { name: "in", index: 0 } }] });
});

test("connection drag validates the current port identity and editable scope", () => {
    assert.throws(() => connectionFromPorts({ ...scope(), editable: false }, "wire", output, input), /read-only/);
    assert.throws(() => connectionFromPorts(scope(), "wire", output, { ...input, name: "renamed" }), /port changed/);
    assert.throws(() => connectionFromPorts(scope(), "wire", output, { ...input, index: 4 }), /port changed/);
    assert.throws(() => connectionFromPorts(scope(), "wire", output, { ...input, address: { instanceId: "gone" } }), /no longer/);
    assert.throws(() => connectionFromPorts(scope(), "wire", output, output), /input and an output/);
});

test("event connection drag retains distinct registered event types for backend conversion", () => {
    const connection = connectionFromPorts(scope(), "events",
        { ...output, kind: "event", name: "notes" }, { ...input, kind: "event", name: "gate" });
    assert.equal(connection.kind, "event");
    assert.equal(connection.sourceType, "midi");
    assert.equal(connection.targetType, "trigger");
    assert.throws(() => connectionFromPorts(scope(), "wire", output, { ...input, kind: "event", name: "gate" }), /same port kind/);
});

const matcher = (port: GraphPortEndpoint): GraphPortMatcher => ({ ...port.address, path: [],
    port: { name: port.name, index: port.index } });
const cInput = { ...input, address: { instanceId: "c" } };
const cOutput = { ...output, address: { instanceId: "c" } };
function connectedScope(): GraphScope {
    const graph = scope();
    const node = structuredClone(graph.nodes[1]);
    node.address = { instanceId: "c" }; node.instance.instanceId = "c";
    graph.nodes.push(node);
    graph.connections = [{ id: "wire", applied: true, connection: {
        connectionId: "wire", kind: "sample", sourceType: "stereo", targetType: "mono",
        outputs: [matcher(output)], inputs: [matcher(input)],
    } }];
    return graph;
}
function selectedEnd(graph: GraphScope, port = input): GraphConnectionEnd {
    return connectionEndsAtPort(graph, port)[0];
}
function replacement(graph: GraphScope, end: GraphConnectionEnd, target: GraphPortEndpoint | null) {
    const mutation = connectionEndMutation(graph, { end, target });
    assert.ok(mutation?.type === "upsert");
    return mutation.connection as GraphScopeConnection["connection"];
}

test("hover feedback permits either drag direction without changing the graph", () => {
    const graph = scope();
    const before = structuredClone(graph);
    for (const [first, second] of [[output, input], [input, output]]) {
        const drag = movePortConnection(beginPortConnection(first, 1, 0, { x: 0, y: 0 })!, 1, 1, { x: 100, y: 50 })!;
        assert.equal(canDropPortConnection(graph, drag, second), true);
        assert.ok(finishPortConnection(drag, 1, drag.end, second));
        assert.equal(canDropPortConnection(graph, drag, first), false);
        assert.equal(canDropPortConnection(graph, drag, { ...second, kind: "event" }), false);
        assert.equal(canDropPortConnection(graph, drag, null), false);
        assert.equal(canDropPortConnection({ ...graph, editable: false }, drag, second), false);
    }
    assert.deepEqual(graph, before);
});

test("reattachment feedback rejects the original port, stale records and shared type changes", () => {
    const graph = connectedScope();
    const end = selectedEnd(graph);
    const drag = beginPortConnection(input, 1, 0, { x: 0, y: 0 }, end)!;
    assert.equal(canDropPortConnection(graph, drag, cInput), true);
    assert.equal(canDropPortConnection(graph, drag, input), false);
    assert.equal(canDropPortConnection(graph, drag, cOutput), false);
    assert.equal(canDropPortConnection(graph, drag, { ...cInput, name: "renamed" }), false);
    graph.nodes[2].sampleInputs[0].channelType = "stereo";
    assert.equal(canDropPortConnection(graph, drag, cInput), true); // Whole-end conversion remains possible.
    // Give the current connection another input. Its shared target type can
    // no longer change for just the moved end. Capture that new record anew.
    graph.connections[0].connection.inputs.push(matcher(cInput));
    const shared = beginPortConnection(input, 1, 0, { x: 0, y: 0 }, selectedEnd(graph))!;
    assert.equal(canDropPortConnection(graph, shared, cInput), false);
    const originalGraph = connectedScope();
    const originalDrag = beginPortConnection(input, 1, 0, { x: 0, y: 0 }, selectedEnd(originalGraph))!;
    const staleGraph = structuredClone(originalGraph);
    staleGraph.connections[0].connection.sourceType = "mono";
    assert.equal(canDropPortConnection(staleGraph, originalDrag, cInput), false);
});

test("pickup returns all connection choices for the clicked port without selecting one", () => {
    const graph = connectedScope();
    assert.deepEqual(connectionEndsAtPort(graph, cInput), []);
    assert.equal(connectionEndsAtPort(graph, input).length, 1);
    graph.connections.push({ ...graph.connections[0], id: "second", connection: {
        ...graph.connections[0].connection, connectionId: "second", outputs: [matcher(cOutput)],
    } });
    assert.deepEqual(connectionEndsAtPort(graph, input).map((end) => end.connection.id), ["wire", "second"]);
    assert.equal(connectionEndsAtPort(graph, { ...input, kind: "event" }).length, 0);
    assert.equal(connectionEndsAtPort(graph, { ...input, name: "renamed" }).length, 0);
    assert.equal(connectionEndsAtPort(graph, { ...input, address: { ...input.address, path: [{ name: "inner" }] } }).length, 0);
});

test("reattachment replaces one end with one upsert and retains connection identity", () => {
    const graph = connectedScope();
    const before = structuredClone(graph);
    const changed = replacement(graph, selectedEnd(graph), cInput);
    assert.equal(changed.connectionId, "wire");
    assert.equal(changed.sourceType, "stereo"); assert.equal(changed.targetType, "mono");
    assert.deepEqual(changed.outputs, [matcher(output)]);
    assert.deepEqual(changed.inputs, [matcher(cInput)]);
    const sourceChanged = replacement(graph, selectedEnd(graph, output), cOutput);
    assert.deepEqual(sourceChanged.outputs, [matcher(cOutput)]);
    assert.deepEqual(sourceChanged.inputs, [matcher(input)]);
    assert.deepEqual(graph, before);
});

test("disconnect deletes only a connection that has no ends left on the selected side", () => {
    const graph = connectedScope();
    const unrelated = { ...graph.connections[0], id: "other" };
    graph.connections.push(unrelated);
    assert.deepEqual(connectionEndMutation(graph, { end: selectedEnd(graph), target: null }), { type: "delete", id: "wire" });
    graph.connections[0].connection.inputs.push(matcher(cInput));
    const retained = replacement(graph, selectedEnd(graph), null);
    assert.deepEqual(retained.inputs, [matcher(cInput)]);
    assert.deepEqual(retained.outputs, [matcher(output)]);
    assert.equal(graph.connections[1], unrelated);
});

test("moving an end of a multi-input or multi-output record preserves its other ends and order", () => {
    const graph = connectedScope();
    graph.connections[0].connection.inputs.push(matcher(cInput));
    graph.connections[0].connection.outputs.push(matcher(cOutput));
    const aInput = { ...input, address: { instanceId: "a" } };
    assert.deepEqual(replacement(graph, selectedEnd(graph), aInput).inputs, [matcher(aInput), matcher(cInput)]);
    assert.deepEqual(replacement(graph, selectedEnd(graph), aInput).outputs, [matcher(output), matcher(cOutput)]);
    const bOutput = { ...output, address: { instanceId: "b" } };
    assert.deepEqual(replacement(graph, selectedEnd(graph, output), bOutput).outputs, [matcher(bOutput), matcher(cOutput)]);
    assert.deepEqual(fixedConnectionPorts(graph, selectedEnd(graph)), [output, cOutput]);
    assert.deepEqual(fixedConnectionPorts(graph, selectedEnd(graph, output)), [input, cInput]);
});

test("editing a wildcard preserves its other matched ports and channel selections", () => {
    const graph = connectedScope();
    graph.nodes[1].sampleInputs.push({ name: "aux", index: 1, channelType: "mono" });
    graph.connections[0].connection.inputs = [{ instanceId: "b", path: [], port: { channel: 0 } }];
    const changed = replacement(graph, selectedEnd(graph), cInput);
    assert.deepEqual(changed.inputs, [
        { ...matcher(cInput), port: { name: "in", index: 0, channel: 0 } },
        { instanceId: "b", path: [], port: { name: "aux", index: 1, channel: 0 } },
    ]);
    const removed = replacement(graph, selectedEnd(graph), null);
    assert.deepEqual(removed.inputs, [changed.inputs[1]]);
});

test("moving a channel-selected source retains the mono connection type on a stereo port", () => {
    const graph = connectedScope();
    graph.connections[0].connection.sourceType = "mono";
    graph.connections[0].connection.outputs[0].port.channel = 1;
    const changed = replacement(graph, selectedEnd(graph, output), cOutput);
    assert.equal(changed.sourceType, "mono");
    assert.deepEqual(changed.outputs, [{ ...matcher(cOutput), port: { name: "out", index: 0, channel: 1 } }]);
    graph.nodes[2].sampleOutputs[0].channelType = "mono";
    assert.equal(canMoveConnectionEnd(graph, selectedEnd(graph, output), cOutput), false);
});

test("a single whole-port end can change type; ends sharing a side cannot disagree on its type", () => {
    const graph = connectedScope();
    graph.nodes[2].sampleInputs[0].channelType = "stereo";
    assert.equal(replacement(graph, selectedEnd(graph), cInput).targetType, "stereo");
    graph.connections[0].connection.inputs.push(matcher({ ...input, address: { instanceId: "a" } }));
    assert.equal(canMoveConnectionEnd(graph, selectedEnd(graph), cInput), false);
    assert.throws(() => replacement(graph, selectedEnd(graph), cInput), /same type/);
});

test("event endpoints reconnect and disconnect with the same rules as sample endpoints", () => {
    const graph = connectedScope();
    const eventOutput = { ...output, kind: "event" as const, name: "notes" };
    const eventInput = { ...input, kind: "event" as const, name: "gate" };
    const nextInput = { ...eventInput, address: { instanceId: "c" } };
    graph.connections[0].connection = { connectionId: "wire", kind: "event", sourceType: "midi", targetType: "trigger",
        outputs: [matcher(eventOutput)], inputs: [matcher(eventInput)] };
    const end = selectedEnd(graph, eventInput);
    const changed = replacement(graph, end, nextInput);
    assert.deepEqual(changed.inputs, [matcher(nextInput)]);
    assert.equal(changed.sourceType, "midi"); assert.equal(changed.targetType, "trigger");
    assert.deepEqual(connectionEndMutation(graph, { end, target: null }), { type: "delete", id: "wire" });
    assert.equal(canMoveConnectionEnd(graph, end, cInput), false);
});

test("a gesture cannot overwrite a changed or missing connection, or use stale ports", () => {
    const graph = connectedScope();
    const end = structuredClone(selectedEnd(graph));
    assert.equal(connectionEndMutation(graph, { end, target: { ...input, address: { ...input.address, path: [] } } }), null);
    graph.connections[0].applied = false; // Application status is not a semantic edit.
    assert.ok(replacement(graph, end, cInput));
    assert.equal(canMoveConnectionEnd({ ...graph, editable: false }, end, cInput), false);
    assert.equal(canMoveConnectionEnd(graph, end, { ...cInput, name: "stale" }), false);
    assert.equal(canMoveConnectionEnd(graph, end, output), false);
    assert.throws(() => connectionEndMutation({ ...graph, connections: [] }, { end, target: null }), /changed/);
    graph.connections[0].connection.inputs.push(matcher(cInput));
    assert.throws(() => connectionEndMutation(graph, { end, target: null }), /changed/);
    assert.equal(canMoveConnectionEnd(graph, end, cInput), false);
});

test("unresolved interior matchers survive an edit to a separate root port", () => {
    const graph = connectedScope();
    const interior = { instanceId: "b", path: [{ name: "inside" }], port: { name: "audio" } };
    graph.connections[0].connection.inputs.push(interior);
    assert.deepEqual(replacement(graph, selectedEnd(graph), cInput).inputs, [matcher(cInput), interior]);
    assert.deepEqual(replacement(graph, selectedEnd(graph), null).inputs, [interior]);
});

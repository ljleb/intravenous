import test from "node:test";
import assert from "node:assert/strict";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../src/graphConnectionEditing";
import type { GraphScope, GraphScopeNode } from "../src/graphScope";
import { beginPortConnection, movePortConnection, finishPortConnection, previewConnectionDetached } from "../src/portConnectionGesture";

const output: GraphPortEndpoint = { address: { instanceId: "source" }, direction: "output", kind: "sample", index: 0, name: "out" };
const input: GraphPortEndpoint = { address: { instanceId: "target" }, direction: "input", kind: "sample", index: 0, name: "in" };
const nextInput = { ...input, address: { instanceId: "another-target" } };
const nextOutput = { ...output, address: { instanceId: "another-source" } };
const end: GraphConnectionEnd = { port: input, connection: { id: "wire", applied: true,
    connection: { kind: "sample", sourceType: "mono", targetType: "mono",
        outputs: [{ ...output.address, port: { name: output.name, index: output.index } }],
        inputs: [{ ...input.address, port: { name: input.name, index: input.index } }] } } };
const elsewhere = { x: 100, y: 30 };
const nodes = [input, output, nextInput, nextOutput].map((port): GraphScopeNode => {
    const sample = { index: port.index, name: port.name, channelType: "mono" };
    return { address: port.address, instance: { instanceId: port.address.instanceId, definitionId: "test.node", realized: true },
        loadState: "ready", hasChildren: false, eventInputs: [], eventOutputs: [],
        sampleInputs: port.direction === "input" ? [sample] : [], sampleOutputs: port.direction === "output" ? [sample] : [] };
});
const graph: GraphScope = { generation: 1, editable: true, nodes, connections: [end.connection] };

test("the cable preview reflects Alt and retains a moved cable's existing detach", () => {
    const drag = beginPortConnection(output, 7, 0, { x: 0, y: 0 })!;
    assert.equal(previewConnectionDetached(drag, null, false), false);
    assert.equal(previewConnectionDetached(drag, input, true), true);
    assert.equal(previewConnectionDetached(drag, input, false), false);

    const detachedEnd: GraphConnectionEnd = { ...end,
        connection: { ...end.connection, connection: { ...end.connection.connection, loopExtraLatency: 8 } } };
    const move = beginPortConnection(input, 7, 0, { x: 0, y: 0 }, detachedEnd)!;
    assert.equal(previewConnectionDetached(move, nextInput, false), true);
});

test("captured port drag connects on release over the opposite endpoint", () => {
    const started = beginPortConnection(output, 7, 0, { x: 10, y: 10 })!;
    const moved = movePortConnection(started, 7, 1, elsewhere)!;
    assert.equal(moved.moved, true);
    assert.deepEqual(finishPortConnection(graph, moved, 7, elsewhere, input), { type: "create", first: output, second: input });
    const reverse = beginPortConnection(input, 8, 0, elsewhere)!;
    assert.deepEqual(finishPortConnection(graph, reverse, 8, { x: 10, y: 10 }, output), { type: "create", first: input, second: output });
});

test("an existing end reconnects to a compatible port in either drag direction", () => {
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    assert.deepEqual(finishPortConnection(graph, drag, 7, elsewhere, nextInput), { type: "edit", edit: { end, target: nextInput } });
    const sourceEnd = { ...end, port: output };
    const sourceDrag = beginPortConnection(output, 7, 0, { x: 10, y: 10 }, sourceEnd)!;
    assert.deepEqual(finishPortConnection(graph, sourceDrag, 7, elsewhere, nextOutput), { type: "edit", edit: { end: sourceEnd, target: nextOutput } });
});

test("dropping an existing end on empty space or an incompatible port disconnects it", () => {
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    const disconnect = { type: "edit", edit: { end, target: null } };
    assert.deepEqual(finishPortConnection(graph, drag, 7, elsewhere, null, true), disconnect);
    assert.deepEqual(finishPortConnection(graph, drag, 7, elsewhere, output), disconnect);
    assert.deepEqual(finishPortConnection(graph, drag, 7, elsewhere, { ...nextInput, kind: "event" }), disconnect);
});

test("releasing without a drag, back on the source, or away from the graph cancels an end move", () => {
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    // Group caps, controls and the outside of the editor are not empty space.
    assert.equal(finishPortConnection(graph, drag, 7, elsewhere, null, false), null);
    assert.equal(finishPortConnection(graph, drag, 7, elsewhere, { ...input, address: { ...input.address, path: [] } }), null);
    assert.equal(finishPortConnection(graph, drag, 8, elsewhere, null, true), null);
    assert.equal(finishPortConnection(graph, drag, 7, { x: 11, y: 10 }, null, true), null);
});

test("moving a connection keeps the press position separate from its fixed end", () => {
    const before = JSON.stringify(end);
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    const moved = movePortConnection(drag, 7, 1, elsewhere)!;
    assert.deepEqual(moved.start, { x: 10, y: 10 });
    assert.equal(moved.edit, end);
    assert.equal(JSON.stringify(end), before); // Pickup and motion make no graph edit.
    assert.equal(movePortConnection(moved, 7, 0, elsewhere), null);
    // A deliberate drag that returns near its start is still a drag.
    assert.equal(movePortConnection(moved, 7, 1, { x: 10, y: 11 })!.moved, true);
});

test("a click, missing target, wrong direction, or wrong kind does not create a connection", () => {
    const drag = beginPortConnection(output, 7, 0, { x: 10, y: 10 })!;
    assert.equal(finishPortConnection(graph, drag, 7, { x: 12, y: 11 }, input), null);
    assert.equal(finishPortConnection(graph, drag, 7, elsewhere, null), null);
    assert.equal(finishPortConnection(graph, drag, 7, elsewhere, output), null);
    assert.equal(finishPortConnection(graph, drag, 7, elsewhere, { ...input, kind: "event" }), null);
});

test("port dragging ignores other pointers and stops when the primary button is lost", () => {
    const drag = beginPortConnection(output, 7, 0, { x: 10, y: 10 })!;
    assert.equal(movePortConnection(drag, 8, 1, { x: 100, y: 30 }), drag);
    assert.equal(finishPortConnection(graph, drag, 8, elsewhere, input), null);
    assert.equal(movePortConnection(drag, 7, 0, { x: 100, y: 30 }), null);
    assert.equal(beginPortConnection(output, 7, 1, { x: 10, y: 10 }), null);
    assert.equal(beginPortConnection(output, 7, 2, { x: 10, y: 10 }), null);
});

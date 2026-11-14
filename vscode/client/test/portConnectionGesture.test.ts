import test from "node:test";
import assert from "node:assert/strict";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../src/graphConnectionEditing";
import { beginPortConnection, movePortConnection, finishPortConnection } from "../src/portConnectionGesture";

const output: GraphPortEndpoint = { address: { instanceId: "source" }, direction: "output", kind: "sample", index: 0, name: "out" };
const input: GraphPortEndpoint = { address: { instanceId: "target" }, direction: "input", kind: "sample", index: 0, name: "in" };

test("captured port drag connects on release over the opposite endpoint", () => {
    const started = beginPortConnection(output, 7, 0, { x: 10, y: 10 })!;
    const moved = movePortConnection(started, 7, 1, { x: 100, y: 30 })!;
    assert.equal(moved.moved, true);
    assert.deepEqual(finishPortConnection(moved, 7, { x: 100, y: 30 }, input), { type: "create", first: output, second: input });
    const reverse = beginPortConnection(input, 8, 0, { x: 100, y: 30 })!;
    assert.deepEqual(finishPortConnection(reverse, 8, { x: 10, y: 10 }, output), { type: "create", first: input, second: output });
});

const end: GraphConnectionEnd = { port: input, connection: { id: "wire", applied: true,
    connection: { kind: "sample", sourceType: "mono", targetType: "mono",
        outputs: [{ ...output.address, port: { name: output.name, index: output.index } }],
        inputs: [{ ...input.address, port: { name: input.name, index: input.index } }] } } };
const elsewhere = { x: 100, y: 30 };

test("an existing end reconnects only to its own direction, in either drag direction", () => {
    const nextInput = { ...input, address: { instanceId: "another-target" } };
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    assert.deepEqual(finishPortConnection(drag, 7, elsewhere, nextInput), { type: "edit", edit: { end, target: nextInput } });
    assert.equal(finishPortConnection(drag, 7, elsewhere, output), null);
    assert.equal(finishPortConnection(drag, 7, elsewhere, { ...nextInput, kind: "event" }), null);
    const sourceEnd = { ...end, port: output };
    const sourceDrag = beginPortConnection(output, 7, 0, { x: 10, y: 10 }, sourceEnd)!;
    const nextOutput = { ...output, address: { instanceId: "another-source" } };
    assert.deepEqual(finishPortConnection(sourceDrag, 7, elsewhere, nextOutput), { type: "edit", edit: { end: sourceEnd, target: nextOutput } });
});

test("only an explicit empty-editor drop disconnects an existing end", () => {
    const drag = beginPortConnection(input, 7, 0, { x: 10, y: 10 }, end)!;
    assert.deepEqual(finishPortConnection(drag, 7, elsewhere, null, true), { type: "edit", edit: { end, target: null } });
    // Group caps, controls and the outside of the editor are not empty space.
    assert.equal(finishPortConnection(drag, 7, elsewhere, null, false), null);
    assert.equal(finishPortConnection(drag, 7, elsewhere, output, true), null);
    assert.equal(finishPortConnection(drag, 7, elsewhere, { ...input, address: { ...input.address, path: [] } }, true), null);
    assert.equal(finishPortConnection(drag, 8, elsewhere, null, true), null);
    assert.equal(finishPortConnection(drag, 7, { x: 11, y: 10 }, null, true), null);
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
    assert.equal(finishPortConnection(drag, 7, { x: 12, y: 11 }, input), null);
    assert.equal(finishPortConnection(drag, 7, { x: 100, y: 30 }, null), null);
    assert.equal(finishPortConnection(drag, 7, { x: 100, y: 30 }, output), null);
    assert.equal(finishPortConnection(drag, 7, { x: 100, y: 30 }, { ...input, kind: "event" }), null);
});

test("port dragging ignores other pointers and stops when the primary button is lost", () => {
    const drag = beginPortConnection(output, 7, 0, { x: 10, y: 10 })!;
    assert.equal(movePortConnection(drag, 8, 1, { x: 100, y: 30 }), drag);
    assert.equal(finishPortConnection(drag, 8, { x: 100, y: 30 }, input), null);
    assert.equal(movePortConnection(drag, 7, 0, { x: 100, y: 30 }), null);
    assert.equal(beginPortConnection(output, 7, 1, { x: 10, y: 10 }), null);
    assert.equal(beginPortConnection(output, 7, 2, { x: 10, y: 10 }), null);
});

import test from "node:test";
import assert from "node:assert/strict";
import { connectionCursorOutputFraction, type PortColorColumns } from "../src/connectionPreview";
import { connectionWirePath } from "../src/selectedLaneConnections";

const columns: PortColorColumns[] = [
    { nodeKey: "wide", inputX: 20, outputX: 200, top: 30, bottom: 50 },
    { nodeKey: "compact", inputX: 150, outputX: 200, top: 180, bottom: 200 },
];

test("cursor colour blends across the input and output columns and clamps outside them", () => {
    const amount = (x: number) => connectionCursorOutputFraction({ x, y: 40 }, columns);
    assert.equal(amount(-100), 0);
    assert.equal(amount(20), 0);
    assert.equal(amount(65), 0.25);
    assert.equal(amount(110), 0.5);
    assert.equal(amount(155), 0.75);
    assert.equal(amount(200), 1);
    assert.equal(amount(500), 1);
});

test("cursor colour follows the nearest row when its port columns have a different layout", () => {
    assert.equal(connectionCursorOutputFraction({ x: 175, y: 190 }, columns), 0.5);
    assert.equal(connectionCursorOutputFraction({ x: 110, y: 190 }, columns), 0);
    assert.equal(connectionCursorOutputFraction({ x: 110, y: 60 }, columns), 0.5);
});

test("missing or coincident columns give a neutral blend instead of an invalid colour", () => {
    assert.equal(connectionCursorOutputFraction({ x: 0, y: 0 }, []), 0.5);
    assert.equal(connectionCursorOutputFraction({ x: 0, y: 0 }, [{ ...columns[0], outputX: 20 }]), 0.5);
});

test("colour interpolation also works if the two columns are reversed", () => {
    const reversed = [{ ...columns[0], inputX: 200, outputX: 20 }];
    assert.equal(connectionCursorOutputFraction({ x: 200, y: 40 }, reversed), 0);
    assert.equal(connectionCursorOutputFraction({ x: 110, y: 40 }, reversed), 0.5);
    assert.equal(connectionCursorOutputFraction({ x: 20, y: 40 }, reversed), 1);
});

const coordinates = (path: string) => path.match(/-?\d+(?:\.\d+)?/g)!.map(Number);

test("preview curves retain output-left and input-right tangents in both drag directions", () => {
    const output = { x: 200, y: 40 };
    const input = { x: 20, y: 140 };
    const forward = coordinates(connectionWirePath(output, input));
    assert.deepEqual(forward.slice(0, 2), [output.x, output.y]);
    assert.ok(forward[2] < output.x);
    assert.equal(forward[3], output.y);
    assert.ok(forward[4] > input.x);
    assert.equal(forward[5], input.y);
    assert.deepEqual(forward.slice(-2), [input.x, input.y]);
    const reverse = coordinates(connectionWirePath(input, output));
    assert.ok(reverse[2] > input.x);
    assert.ok(reverse[4] < output.x);
    assert.deepEqual(reverse.slice(0, 2), [input.x, input.y]);
    assert.deepEqual(reverse.slice(-2), [output.x, output.y]);
});

test("aligned endpoints use the same cubic without a detour or overshoot", () => {
    const points = coordinates(connectionWirePath({ x: 200, y: 40 }, { x: 20, y: 40 }));
    const heights = points.filter((_, index) => index % 2 === 1);
    const horizontal = points.filter((_, index) => index % 2 === 0);
    assert.ok(heights.every((y) => y === 40));
    assert.ok(horizontal.every((x) => x >= 20 && x <= 200));
    assert.deepEqual(points.slice(0, 2), [200, 40]);
    assert.deepEqual(points.slice(-2), [20, 40]);
});

test("reversing the endpoints reverses one curve instead of changing its route", () => {
    const first = { x: 200, y: 40 };
    const second = { x: 20, y: 140 };
    const forward = coordinates(connectionWirePath(first, second));
    const backward = coordinates(connectionWirePath(second, first));
    const pairs: number[][] = [];
    for (let index = 0; index < forward.length; index += 2) pairs.push(forward.slice(index, index + 2));
    assert.deepEqual(backward, pairs.reverse().flat());
});

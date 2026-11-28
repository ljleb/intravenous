import test from "node:test";
import assert from "node:assert/strict";
import { lanePortContinuationColumn, lanePortControlColumnWidth, lanePortControls, lanePortFoldedSocketHeight, lanePortLayout, lanePortRowHeight } from "../src/lanePortLayout";
import { minimumLaneHeight } from "../src/timelineViewport";

test("port controls use narrow knobs before falling back to sockets alone", () => {
    const narrow = { knobSize: 20, handleWidth: 14 };
    assert.equal(lanePortControls(91, 24), null);
    assert.deepEqual(lanePortControls(92, 24), narrow);
    assert.deepEqual(lanePortControls(92, 31), narrow);
    assert.deepEqual(lanePortControls(119, 24), narrow);
    assert.deepEqual(lanePortControls(120, 24), { knobSize: 24, handleWidth: 24 });
    assert.deepEqual(lanePortControls(133, 31), narrow);
    assert.deepEqual(lanePortControls(134, 31), { knobSize: 31, handleWidth: 24 });
});

test("socket columns reserve room for the largest knob the header width can fit", () => {
    for (const [width, column] of [[91, 0], [92, 30], [119, 30], [120, 34], [133, 34], [134, 41]]) {
        assert.equal(lanePortControlColumnWidth(width), column);
    }
});

test("a short editable header fits its first row above the folded socket", () => {
    const ports = ["audio", "cutoff"];
    for (const [height, rowHeight] of [[28, 20], [31.5, 20], [32, 24], [39, 31]]) {
        assert.equal(lanePortRowHeight(height), rowHeight);
        const layout = lanePortLayout(ports, height, lanePortRowHeight(height), lanePortFoldedSocketHeight);
        assert.deepEqual(layout.direct, ["audio"]);
        assert.deepEqual(layout.folded, ["cutoff"]);
    }
});

test("a narrow continuation grows away from its socket edge when there is room", () => {
    const column = { left: 12, width: 54 };
    assert.deepEqual(lanePortContinuationColumn(column, 400, "input"), { left: 12, width: 144 });
    assert.deepEqual(lanePortContinuationColumn({ ...column, left: 150 }, 400, "output"), { left: 60, width: 144 });
    assert.deepEqual(lanePortContinuationColumn(column, 100, "output"), { left: 4, width: 92 });
    assert.deepEqual(lanePortContinuationColumn({ left: 12, width: 160 }, 400, "input"), { left: 12, width: 160 });
});

test("lane ports unfold at the height needed for the complete direction", () => {
    const ports = ["audio", "cutoff"];
    assert.deepEqual(lanePortLayout(ports, 46), { direct: ports.slice(0, 1), folded: ports.slice(1), continuationOffset: 25 });
    assert.deepEqual(lanePortLayout(ports, 47), { direct: ports, folded: [], continuationOffset: 50 });
});

test("overflow keeps the rows that fit in definition order", () => {
    const ports = ["audio", "cutoff", "resonance", "mix"];
    assert.deepEqual(lanePortLayout(ports, 72), { direct: ports.slice(0, 3), folded: ports.slice(3), continuationOffset: 75 });
    assert.deepEqual(lanePortLayout(ports, 97), { direct: ports, folded: [], continuationOffset: 100 });
    assert.deepEqual(lanePortLayout(["out"], 72), { direct: ["out"], folded: [], continuationOffset: 25 });
});

test("the folded socket reserves space only while the column has hidden ports", () => {
    const ports = ["audio", "trigger", "mix"];
    const fit = (height: number) => lanePortLayout(ports, height, 24, 8);
    assert.deepEqual(fit(31), { direct: [], folded: ports, continuationOffset: 0 });
    assert.deepEqual(fit(32), { direct: ports.slice(0, 1), folded: ports.slice(1), continuationOffset: 27 });
    assert.deepEqual(fit(58), { direct: ports.slice(0, 1), folded: ports.slice(1), continuationOffset: 27 });
    assert.deepEqual(fit(59), { direct: ports.slice(0, 2), folded: ports.slice(2), continuationOffset: 54 });
    assert.deepEqual(fit(78), { direct: ports, folded: [], continuationOffset: 81 });
});

test("folded columns resume after their own last visible row", () => {
    const ports = ["first", "second", "third"];
    const inputs = lanePortLayout(ports, 65, 31, 8);
    const outputs = lanePortLayout(ports.slice(0, 2), 65, 31, 8);
    assert.equal(inputs.continuationOffset, 34);
    assert.equal(outputs.continuationOffset, 68);
});

test("at minimum lane height the compact handle represents all ports", () => {
    const ports = ["frequency", "detune", "phase"];
    assert.deepEqual(lanePortLayout(ports, minimumLaneHeight - 1), { direct: [], folded: ports, continuationOffset: 0 });
    assert.deepEqual(lanePortLayout(ports, 22), { direct: ports.slice(0, 1), folded: ports.slice(1), continuationOffset: 25 });
});

test("small and unmeasured lanes keep every port reachable without changing order", () => {
    for (const height of [0, -1, NaN, 22, 47, 72, 97, 500]) {
        for (let count = 0; count < 30; ++count) {
            const ports = Array.from({ length: count }, (_, i) => i);
            const { direct, folded } = lanePortLayout(ports, height);
            assert.deepEqual([...direct, ...folded], ports);
            if (folded.length) assert.ok(direct.length < count);
        }
    }
});

test("event-only and mixed columns use the same row spacing and continuation offset", () => {
    const inputs = ["audio", "trigger", "mix"];
    const outputs = ["trigger", "pulse", "release"];
    for (const rowHeight of [24, 31]) {
        const height = 2 * rowHeight + 3 + 8;
        const input = lanePortLayout(inputs, height, rowHeight, 8);
        const output = lanePortLayout(outputs, height, rowHeight, 8);
        assert.deepEqual(input.direct, inputs.slice(0, 2));
        assert.deepEqual(output.direct, outputs.slice(0, 2));
        assert.equal(output.continuationOffset, 2 * (rowHeight + 3));
        assert.equal(input.continuationOffset, output.continuationOffset);
    }
});

import test from "node:test";
import assert from "node:assert/strict";
import { lanePortLayout } from "../src/lanePortLayout";
import { minimumLaneHeight } from "../src/timelineViewport";

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
    const ports = [24, 22, 24];
    const fit = (height: number) => lanePortLayout(ports, height, (rowHeight) => rowHeight, 8);
    assert.deepEqual(fit(31), { direct: [], folded: ports, continuationOffset: 0 });
    assert.deepEqual(fit(32), { direct: [24], folded: [22, 24], continuationOffset: 27 });
    assert.deepEqual(fit(56), { direct: [24], folded: [22, 24], continuationOffset: 27 });
    assert.deepEqual(fit(57), { direct: [24, 22], folded: [24], continuationOffset: 52 });
    assert.deepEqual(fit(76), { direct: ports, folded: [], continuationOffset: 79 });
});

test("folded columns start at their own next row, even with the same header bottom", () => {
    const inputs = lanePortLayout([31, 31, 31], 65, (height) => height, 8);
    const outputs = lanePortLayout([22, 22, 22], 65, (height) => height, 8);
    assert.equal(inputs.continuationOffset, 34);
    assert.equal(outputs.continuationOffset, 50);
    assert.equal(lanePortLayout([31, 31, 31], 73, (height) => height, 8).continuationOffset, 68);
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

test("inline knobs reserve their full row height before exposing more ports", () => {
    const ports = [{ name: "audio", height: 30 }, { name: "trigger", height: 22 }, { name: "mix", height: 30 }];
    const heightOf = (port: typeof ports[number]) => port.height;
    assert.deepEqual(lanePortLayout(ports, 54, heightOf), { direct: ports.slice(0, 1), folded: ports.slice(1), continuationOffset: 33 });
    assert.deepEqual(lanePortLayout(ports, 55, heightOf), { direct: ports.slice(0, 2), folded: ports.slice(2), continuationOffset: 58 });
    assert.deepEqual(lanePortLayout(ports, 88, heightOf), { direct: ports, folded: [], continuationOffset: 91 });
    assert.deepEqual(lanePortLayout(ports.slice(0, 1), 29, heightOf), { direct: [], folded: ports.slice(0, 1), continuationOffset: 0 });
    assert.deepEqual(lanePortLayout(ports.slice(0, 1), 30, heightOf), { direct: ports.slice(0, 1), folded: [], continuationOffset: 33 });
});

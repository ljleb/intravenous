import test from "node:test";
import assert from "node:assert/strict";
import { lanePortLayout } from "../src/lanePortLayout";

test("lane ports unfold at the height needed for the complete direction", () => {
    const ports = ["audio", "cutoff"];
    assert.deepEqual(lanePortLayout(ports, 46), { direct: [], folded: ports });
    assert.deepEqual(lanePortLayout(ports, 47), { direct: ports, folded: [] });
});

test("overflow reserves a handle after the visible ports in definition order", () => {
    const ports = ["audio", "cutoff", "resonance", "mix"];
    assert.deepEqual(lanePortLayout(ports, 72), { direct: ports.slice(0, 2), folded: ports.slice(2) });
    assert.deepEqual(lanePortLayout(ports, 97), { direct: ports, folded: [] });
    assert.deepEqual(lanePortLayout(["out"], 72), { direct: ["out"], folded: [] });
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
    assert.deepEqual(lanePortLayout(ports, 79, heightOf), { direct: ports.slice(0, 1), folded: ports.slice(1) });
    assert.deepEqual(lanePortLayout(ports, 80, heightOf), { direct: ports.slice(0, 2), folded: ports.slice(2) });
    assert.deepEqual(lanePortLayout(ports, 88, heightOf), { direct: ports, folded: [] });
    assert.deepEqual(lanePortLayout(ports.slice(0, 1), 29, heightOf), { direct: [], folded: ports.slice(0, 1) });
    assert.deepEqual(lanePortLayout(ports.slice(0, 1), 30, heightOf), { direct: ports.slice(0, 1), folded: [] });
});

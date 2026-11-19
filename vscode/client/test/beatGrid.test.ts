import assert from "node:assert/strict";
import test from "node:test";
import { beatBpm, beatGridTicks, maximumBeatBpm, maximumBeatSubdivisionCount, readBeatGrid } from "../src/beatGrid";
import { BeatGridEditing } from "../src/beatGridEditing";
import { SetBeatGrid } from "../src/generated/beatsEvents";
import type { GraphScope } from "../src/graphScope";
import { KnobScrub } from "../src/knobScrub";
import { parseKnobValue } from "../src/knobValueEntry";
import { serializeNodeEvent, type JsonValue } from "../src/nodeEvents";

test("the Beats preview draws the default hierarchy from accepted empty arguments", () => {
    const grid = readBeatGrid([])!;
    const ticks = beatGridTicks(grid, { beginSeconds: 0, spanSeconds: 2.01 }, 48000, 500);
    assert.deepEqual(ticks.slice(0, 5).map(({ sample, depth }) => [sample, depth]), [
        [0n, 0], [6000n, 3], [12000n, 2], [18000n, 3], [24000n, 1],
    ]);
    assert.deepEqual(ticks.filter(({ bar }) => bar !== undefined).map(({ sample, bar }) => [sample, bar]),
        [[0n, 1n], [96000n, 2n]]);
});

test("the Beats preview keeps the same rounded sample positions after a pan", () => {
    const grid = readBeatGrid([120, 3, [2]])!;
    const read = (begin: number, end: number) => beatGridTicks(grid,
        { beginSeconds: begin / 9, spanSeconds: (end - begin) / 9 }, 9, (end - begin) * 16)
        .map(({ sample, depth }) => [sample, depth]);
    const marks = read(-8, 10);
    assert.deepEqual(marks, [[-7n, 2], [-4n, 1], [-2n, 2], [0n, 0], [2n, 2], [5n, 1], [7n, 2], [9n, 1]]);
    assert.deepEqual([...read(-8, -3), ...read(-3, 4), ...read(4, 10)], marks);
});

test("zooming out hides dense subdivisions before beats and keeps the bar origin", () => {
    const grid = readBeatGrid([120, 3, [2, 3]])!;
    const view = (spanSeconds: number) => beatGridTicks(grid, { beginSeconds: 0, spanSeconds }, 48000, 200);
    const medium = view(3);
    assert.ok(medium.some(({ depth }) => depth === 2));
    assert.ok(medium.every(({ depth }) => depth < 3));
    const wide = view(1_000_000);
    assert.ok(wide.some(({ sample, bar }) => sample === 0n && bar === 1n));
    assert.ok(wide.every(({ depth }) => depth === 0));
    assert.ok(wide.length < 30);
});

test("the Beats preview uses the signed grid phase when the timeline wraps", () => {
    const grid = readBeatGrid([60, 3, []])!;
    const ticks = beatGridTicks(grid, { beginSeconds: 2 ** 63, spanSeconds: 4 }, 1, 100);
    const first = -(1n << 63n);
    assert.deepEqual(ticks.map(({ x, sample, depth }) => [x, sample, depth]), [
        [0, first, 1], [25, first + 1n, 1], [50, first + 2n, 0], [75, first + 3n, 1],
    ]);
});

test("the settings limit and grid agree on the finest sample interval", () => {
    assert.equal(maximumBeatBpm(7, 4), 105);
    const grid = readBeatGrid([])!;
    assert.equal(maximumBeatSubdivisionCount(grid, 7), 3);
    assert.deepEqual(beatGridTicks(grid, { beginSeconds: 0, spanSeconds: 1 }, 7, 200), []);
});

test("the Beats settings event retains the order of subdivision factors", () => {
    const settings = { bpm: 123.5, beats_per_bar: 3, subdivisions: [3, 2] };
    assert.deepEqual(serializeNodeEvent(SetBeatGrid(settings)).payload, settings);
});

const address = { instanceId: "beats" };

test("Beat knobs use whole steps until BPM sensitivity is reduced", () => {
    const tempo = { min: .000001, max: 4294.967295, wholeByDefault: true };
    const scrub = new KnobScrub(.5, 0, 120, tempo);
    assert.equal(scrub.move(1, 120), 121);
    scrub.changeSensitivity(1, 121);
    assert.equal(scrub.move(1, 121), 121);
    assert.equal(scrub.move(2, 121), 121.5);
    const coarse = new KnobScrub(.5, 0, 120, tempo);
    assert.equal(coarse.move(-300, 120), 1);
    assert.equal(coarse.move(9000, 1), 4294);
    const subdivision = { min: 2, max: 8, integer: true };
    const factor = new KnobScrub(.5, 0, 2, subdivision);
    factor.changeSensitivity(1, 2);
    assert.equal(factor.move(1, 2), 2);
    assert.equal(parseKnobValue("121.25", tempo), 121.25);
    assert.equal(parseKnobValue("2.5", subdivision), "Enter a whole number.");
});

test("the tempo detent keeps its remaining drag travel when sensitivity changes", () => {
    const tempo = { min: .000001, max: 4294.967295, defaultValue: 120, wholeByDefault: true };
    const scrub = new KnobScrub(1, 0, 118, tempo);
    assert.equal(scrub.move(1, 118), 119);
    assert.equal(scrub.move(2, 119), 120);
    assert.equal(scrub.move(11, 120), 120);
    scrub.changeSensitivity(1, 120);
    assert.equal(scrub.move(26, 120), 120);
    assert.equal(scrub.move(27, 120), 120.5);
});

test("a whole-number subdivision knob adds a detent at its minimum default", () => {
    const factor = new KnobScrub(1, 0, 2, { min: 2, max: 8, defaultValue: 2, integer: true });
    assert.equal(factor.move(12, 2), 2);
    assert.equal(factor.move(13, 2), 3);
    assert.equal(factor.move(12, 3), 2);
});

test("a fractional authored default is exact during a whole-step drag", () => {
    const scrub = new KnobScrub(.125, 0, 1.25, { min: 0, max: 8, defaultValue: 1.25, wholeByDefault: true });
    assert.equal(scrub.move(-12, 1.25), 1.25);
    assert.equal(scrub.move(-13, 1.25), 1);
    assert.equal(scrub.move(12, 1), 1.25);
    assert.equal(scrub.move(13, 1.25), 1.25);
    assert.equal(scrub.move(14, 1.25), 2);
});

function scope(revision: number, bpm: number): GraphScope {
    const definitionId = "iv.builtin.beats";
    return { sessionRevision: revision, generation: revision, sampleRate: 48000, editable: true, connections: [],
        nodes: [{ address, loadState: "ready", instance: { instanceId: address.instanceId, definitionId, realized: true },
            construction: { definitionId, arguments: [bpm] }, hasChildren: false,
            sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [] }] };
}

test("the BPM gesture coalesces updates and holds the final grid until the accepted read", async () => {
    const received: JsonValue[] = [];
    let scheduled: (() => void) | undefined, closed = 0;
    let resolveFirst!: () => void;
    const first = new Promise<void>((resolve) => { resolveFirst = resolve; });
    const editing = new BeatGridEditing({ gesture: () => ({
        send: async (event) => {
            received.push(serializeNodeEvent(event).payload);
            if (received.length === 1) await first;
        },
        finish: async () => { ++closed; },
        acceptedRevision: () => 3,
    }) }, () => {}, { set: (callback) => { scheduled = callback; return 1; }, clear: () => { scheduled = undefined; } });
    editing.reconcile(scope(1, 120));
    editing.update(address, readBeatGrid([130])!, true);
    scheduled!();
    await Promise.resolve();
    editing.update(address, readBeatGrid([140])!, true);
    editing.update(address, readBeatGrid([150])!, true);
    const finished = editing.finish(address);
    resolveFirst();
    await finished;
    assert.deepEqual(received, [130, 150].map((bpm) => ({ bpm, beats_per_bar: 4, subdivisions: [2, 2] })));
    assert.equal(closed, 1);
    editing.reconcile(scope(2, 130));
    assert.equal(beatBpm(editing.view(address)!), 150);
    editing.reconcile(scope(4, 160));
    assert.equal(beatBpm(editing.view(address)!), 160);
});

test("a rejected settings event restores accepted settings and closes its undo group", async () => {
    let closed = 0;
    const editing = new BeatGridEditing({ gesture: () => ({
        send: async () => { throw new Error("Settings conflict"); },
        finish: async () => { ++closed; },
    }) }, () => {});
    editing.reconcile(scope(1, 120));
    editing.update(address, readBeatGrid([180])!);
    assert.equal(beatBpm(editing.view(address)!), 180);
    await editing.finish(address);
    assert.equal(beatBpm(editing.view(address)!), 120);
    assert.equal(editing.error(address), "Settings conflict");
    assert.equal(closed, 1);
});

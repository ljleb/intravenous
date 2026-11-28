import assert from "node:assert/strict";
import test from "node:test";
import { beatBpm, beatGridTicks, beatObservationMarks, beatRulerLabels, customBeatRulerLabels, projectBeatMarks,
    maximumBeatBpm, maximumBeatSubdivisionCount, readBeatGrid } from "../src/beatGrid";
import { BeatGridEditing } from "../src/beatGridEditing";
import { SetBeatGrid } from "../src/generated/beatsEvents";
import type { GraphScope } from "../src/graphScope";
import { KnobScrub } from "../src/knobScrub";
import { parseKnobValue } from "../src/knobValueEntry";
import { serializeNodeEvent, type JsonValue } from "../src/nodeEvents";
import { firstSignedSample, shiftSamplePosition, timelineSampleCount } from "../src/timelineSamples";
import { snapTimelineSample } from "../src/timelineGeometry";
import { playheadTimeDrag } from "../src/timelineTimeDrag";
import { detentHalfWidth } from "../src/scrubSensitivity";

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

test("ruler labels use absolute bar and beat numbers at rounded sample positions", () => {
    const grid = readBeatGrid([120, 3, [2]])!;
    const marks = [
        { index: -4n, depth: 1, x: 0 },
        { index: -2n, depth: 2, x: 24 },
        { index: 0n, depth: 0, x: 48 },
        { index: 5n, depth: 1, x: 96 },
        { index: 8n, depth: 1, x: 120 }, // Not on the accepted grid.
        { index: 9n, depth: 1, x: 144 },
        { index: 14n, depth: 0, x: 192 },
    ];
    assert.deepEqual(beatRulerLabels(grid, marks, 9, 250), [
        { x: 0, text: "0.3" }, { x: 48, text: "1" }, { x: 96, text: "1.2" },
        { x: 144, text: "1.3" }, { x: 192, text: "2" },
    ]);
});

test("ruler labels keep bar numbers when the adjacent beats are crowded", () => {
    const grid = readBeatGrid([120, 3, []])!;
    const marks = [
        { index: 0n, depth: 0, x: 0 },
        { index: 24000n, depth: 1, x: 20 },
        { index: 48000n, depth: 1, x: 40 },
        { index: 72000n, depth: 0, x: 50 },
        { index: 96000n, depth: 1, x: 80 },
    ];
    assert.deepEqual(beatRulerLabels(grid, marks, 48000, 105), [{ x: 0, text: "1" }, { x: 50, text: "2" }]);
});

test("Custom ruler labels use each Region's start and keep their numbers when a query skips bars", () => {
    const positive = { source: 1, regionIndex: 1, regionBegin: "0", regionLength: "1000", eventCount: "20",
        depth: 0, firstEventIndex: "4", step: "4", samples: ["201", "404", "602"] };
    const marks = beatObservationMarks({
        marks: [{ index: "10", depth: 0 }],
        beatRuns: [
            { ...positive, regionIndex: 0, regionBegin: "-1000", regionLength: "999", firstEventIndex: "15", samples: ["-207", "-47"] },
            positive,
            { ...positive, source: 2, regionBegin: "200", firstEventIndex: "0", samples: ["250"] },
            { ...positive, depth: 1, samples: ["223"] },
        ],
    });
    assert.deepEqual(marks, [
        { index: "10", depth: 0 },
        { index: "-207", depth: 0, bar: "16" }, { index: "-47", depth: 0, bar: "20" },
        { index: "201", depth: 0, bar: "5" }, { index: "404", depth: 0, bar: "9" },
        { index: "602", depth: 0, bar: "13" },
        { index: "250", depth: 0, bar: "1" }, { index: "223", depth: 1 },
    ]);
    assert.deepEqual(beatObservationMarks({ beatRuns: [{ ...positive, firstEventIndex: "8", samples: ["404"] }] }),
        [{ index: "404", depth: 0, bar: "9" }]);
    assert.deepEqual(customBeatRulerLabels(marks.map(({ index, ...mark }) => ({ ...mark,
        index: BigInt(index), x: (Number(index) + 210) / 2 })), 300), [
        { x: 1.5, text: "16", key: "-207" }, { x: 81.5, text: "20", key: "-47" },
        { x: 205.5, text: "5", key: "201" }, { x: 230, text: "1", key: "250" },
    ]);
});

test("Custom labels on either side of zero match Beats, including when the observation skips bars", () => {
    const half = timelineSampleCount / 2n;
    const period = 96000n;
    const count = half / period;
    const positive = { source: 1, regionIndex: 1, regionBegin: "0", regionLength: String(half),
        eventCount: String(count + 1n), depth: 0, firstEventIndex: "0", step: "2", samples: ["0", "192000"] };
    const negative = { ...positive, regionIndex: 0, regionBegin: String(-half), eventCount: String(count),
        firstEventIndex: String(count - 3n), samples: ["-288000", "-96000"] };
    const marks = beatObservationMarks({ beatRuns: [negative, positive] })
        .map(({ index, ...mark }, slot) => ({ ...mark, index: BigInt(index), x: slot * 50 }));
    const custom = customBeatRulerLabels(marks, 250).map(({ text }) => text);
    assert.deepEqual(custom, ["-2", "0", "1", "3"]);
    assert.deepEqual(custom, beatRulerLabels(readBeatGrid([])!, marks, 48000, 250).map(({ text }) => text));
});

test("Custom ruler labels count backward from a Region's own halfway point", () => {
    const half = 1n << 63n;
    const origin = half - 10n;
    const samples = [20n, half - 1n, half, half + 20n, half + 100n]
        .map((offset) => String(BigInt.asIntN(64, origin + offset)));
    const run = { source: 1, regionIndex: 0, regionBegin: String(origin), regionLength: String(half + 101n),
        eventCount: "10", depth: 0, firstEventIndex: "1", step: "2", samples };
    assert.deepEqual(beatObservationMarks({ beatRuns: [run] }).map(({ bar }) => bar), ["2", "4", "-4", "-2", "0"]);
    assert.deepEqual(beatObservationMarks({ beatRuns: [{ ...run, firstEventIndex: "7", samples: [samples[3]] }] }),
        [{ index: samples[3], depth: 0, bar: "-2" }]);
});

test("observed Beats keep ruler and snap order when the viewport pans through the signed seam", () => {
    const first = firstSignedSample, last = -first - 1n;
    const origin = { index: last - 2n, fraction: 0 };
    const span = 8;
    const run = { source: 1, regionIndex: 0, regionBegin: String(last - 6n), regionLength: "16",
        eventCount: "4", depth: 0, firstEventIndex: "1", step: "1", samples: [String(last), String(first + 2n)] };
    const marks = beatObservationMarks({ beatRuns: [run], marks: [
        { index: String(first + 1n), depth: 2 },
        { index: String(last - 2n), depth: 1 },
        { index: String(first + 2n), depth: 1 },
        { index: String(last), depth: 3 },
    ] });
    const displayed = projectBeatMarks(marks, { origin, span }, 400);
    assert.deepEqual(displayed.map(({ index, depth, x }) => [index, depth, x]), [
        [last - 2n, 1, 0], [last, 0, 100], [first + 1n, 2, 200], [first + 2n, 0, 250],
    ]);
    assert.deepEqual(customBeatRulerLabels(displayed, 400).map(({ text }) => text), ["2", "3"]);
    const targets = displayed.map(({ index }) => index);
    assert.equal(snapTimelineSample(last - 1n, targets, 1), last);
    assert.equal(snapTimelineSample(first, targets, 1), first + 1n);
    const drag = playheadTimeDrag(last - 1n, { spanSamples: span, width: 400 }, () => targets);
    assert.equal(drag(50).snappedIndex, last);
    assert.equal(drag(100).sampleIndex, first);
    assert.equal(drag(150 + 2 * detentHalfWidth).snappedIndex, first + 1n);

    const panned = projectBeatMarks(marks, { origin: shiftSamplePosition(origin, 3), span }, 400);
    assert.deepEqual(panned.map(({ index, depth, x }) => [index, depth, x]), [
        [first + 1n, 2, 50], [first + 2n, 0, 100],
    ]);
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

test("an off-screen Beats window retains exact sample positions beyond the precision of seconds", () => {
    const grid = readBeatGrid([60, 4, []])!;
    const first = (1n << 53n) + 1n;
    const ticks = beatGridTicks(grid, { beginSample: first, spanSeconds: 4 }, 1, 80);
    assert.deepEqual(ticks.map(({ x, sample }) => [x, sample]), [
        [0, first], [20, first + 1n], [40, first + 2n], [60, first + 3n],
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
    const definitionId = "builtin.beats";
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
    assert.equal(editing.draft(address), undefined);
    editing.update(address, readBeatGrid([130])!, true);
    assert.equal(beatBpm(editing.draft(address)!), 130);
    assert.deepEqual(received, []);
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
    assert.equal(beatBpm(editing.draft(address)!), 150);
    editing.reconcile(scope(4, 160));
    assert.equal(beatBpm(editing.view(address)!), 160);
    assert.equal(editing.draft(address), undefined);
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

test("a full lap keeps the beat preview bounded and includes both sides of the signed seam", () => {
    const rate = 48000;
    const grid = readBeatGrid([120, 4, [2]])!;
    const ticks = beatGridTicks(grid, { beginSample: 0n, spanSeconds: Number(timelineSampleCount) / rate }, rate, 1000);
    assert.ok(ticks.some(({ sample, x }) => sample > 0n && x < 500));
    assert.ok(ticks.some(({ sample, x }) => sample < 0n && x >= 500));
    assert.ok(ticks.length <= 126);
    assert.equal(new Set(ticks.map(({ sample }) => sample)).size, ticks.length);
});

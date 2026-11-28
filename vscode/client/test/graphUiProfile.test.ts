import assert from "node:assert/strict";
import test from "node:test";
import { GraphUiTimingSamples, GraphUiTimings, graphUiProfileReport } from "../src/graphUiProfile";

test("graph UI timing preserves short stalls and bounds the percentile sample", () => {
    const timings = new GraphUiTimingSamples();
    for (let ms = 20; ms >= 1; --ms) timings.add(ms);
    assert.deepEqual(timings.summary(), { count: 20, totalMs: 210, p95Ms: 19, maxMs: 20, sampled: 20 });

    for (let index = 0; index < 4500; ++index) timings.add(1);
    assert.deepEqual(timings.summary(), { count: 4520, totalMs: 4710, p95Ms: 1, maxMs: 20, sampled: 4096 });
});

test("graph UI report highlights frame stalls and ranks operations by total time", () => {
    const work = new GraphUiTimings();
    work.record("Build lane list", 3);
    for (let count = 0; count < 10; ++count) work.record("Measure and route wires", 2);
    const intervals = new GraphUiTimingSamples();
    intervals.add(16);
    intervals.add(62);
    const host = new GraphUiTimings();
    host.record("Read graph scope", 36);
    const report = graphUiProfileReport({
        durationMs: 10_000, frameIntervals: intervals.summary(), framesOver25Ms: 1, framesOver50Ms: 1,
        longAnimationFrames: null, work: work.summary(), lanes: 30, expandedModules: 2, connections: 40,
        drawnWires: 14, laneHeight: 20,
    }, host.summary()).join("\n");
    assert.match(report, /Frame intervals: 2, p95 62.00 ms, max 62.00 ms, 1 over 25 ms, 1 over 50 ms/);
    assert.match(report, /Measure and route wires: 10 calls, 20.00 ms total[^\n]+\n +Build lane list: 1 call,/);
    assert.match(report, /Extension host waits[^\n]+\n +Read graph scope: 1 call, 36.00 ms total/);
});

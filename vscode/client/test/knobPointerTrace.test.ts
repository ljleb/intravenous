import test from "node:test";
import assert from "node:assert/strict";
import { KnobPointerTrace, type KnobPointerTraceReport } from "../src/knobPointerTrace";

test("the palette action arms a trace without recording unrelated UI activity", () => {
    const reports: KnobPointerTraceReport[] = [];
    let time = 0;
    const trace = new KnobPointerTrace((report) => reports.push(report), () => time);
    trace.arm({ browser: "test" }, "2026-11-14T00:00:00Z");
    trace.record("keydown", {});
    assert.equal(trace.armed(), true);
    assert.equal(trace.capturing(), false);
    time = 500;
    trace.begin({ inputName: "cutoff" }, { locked: false });
    time = 510;
    trace.record("pointerlockchange", { locked: true });
    time = 600;
    trace.finish("pointer-lock-lost", { locked: false });
    trace.finish("duplicate-stop");
    assert.equal(reports.length, 1);
    assert.equal(reports[0].reason, "pointer-lock-lost");
    assert.deepEqual(reports[0].events.map((event) => event.timeMs), [0, 10, 100]);
    assert.deepEqual(reports[0].events.map((event) => event.type), ["drag-start", "pointerlockchange", "trace-finished"]);
    assert.deepEqual(reports[0].control, { inputName: "cutoff" });
    assert.equal(trace.armed(), false);
});

test("a long trace retains acquisition events and the newest motion in chronological order", () => {
    let report: KnobPointerTraceReport;
    let time = 0;
    const trace = new KnobPointerTrace((value) => { report = value; }, () => time++, 6);
    trace.arm({}); trace.begin("gain", {});
    for (let i = 0; i < 20; ++i) trace.record("mousemove", { i });
    trace.finish("mouse-release");
    assert.equal(report.events.length, 6);
    assert.equal(report.events[0].type, "drag-start");
    assert.deepEqual(report.events.slice(0, 3).map((event) => event.details.i), [undefined, 0, 1]);
    assert.deepEqual(report.events.slice(3).map((event) => event.details.i), [18, 19, undefined]);
    assert.equal(report.events.at(-1).type, "trace-finished");
    assert.equal(report.droppedEvents, 16);
    assert.deepEqual(report.events.map((event) => event.timeMs), report.events.map((event) => event.timeMs).sort((a, b) => a - b));
});

test("manual stop works before the first drag and another recording starts cleanly", () => {
    const reports: KnobPointerTraceReport[] = [];
    const trace = new KnobPointerTrace((report) => reports.push(report), () => 0);
    trace.arm({}); trace.finish("manual-stop");
    assert.deepEqual(reports[0].events, []);
    trace.arm({}); trace.begin("next", {}); trace.finish("mouse-release");
    assert.equal(reports[1].control, "next");
    assert.equal(reports[1].droppedEvents, 0);
});

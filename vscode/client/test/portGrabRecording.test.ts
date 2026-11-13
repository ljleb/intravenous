import test from "node:test";
import assert from "node:assert/strict";
import { PortGrabRecorder } from "../src/portGrabRecording";

// These tests check report classification. Native window activation must be
// recorded in VS Code; synthetic input here cannot reproduce that transition.
test("port recording distinguishes a missing first press from a rejected gesture", () => {
    const recorder = new PortGrabRecorder(() => 10);
    recorder.start("test");
    recorder.record({ type: "blur", focused: false });
    recorder.record({ type: "focus", focused: true });
    recorder.record({ type: "pointermove", focused: true, buttons: 1 });
    recorder.record({ type: "pointerup", focused: true, button: 0 });
    const missing = recorder.finish()!;
    assert.equal(missing.result, "pointerdown-not-observed");
    assert.equal(missing.observedWindowBlur, true);
    assert.equal(missing.observedWindowFocus, true);

    recorder.start("test");
    recorder.record({ type: "pointerdown", focused: true, button: 0, port: "out" });
    recorder.record({ type: "start-rejected", focused: true, reason: "source-not-visible" });
    const rejected = recorder.finish()!;
    assert.equal(rejected.result, "gesture-not-started");
    assert.equal(rejected.events[1].reason, "source-not-visible");
});

test("port recording reports unexpected cancellation and keeps its reason", () => {
    const recorder = new PortGrabRecorder();
    recorder.start("test");
    recorder.record({ type: "pointerdown", focused: true, button: 0, port: "out" });
    recorder.record({ type: "gesture-start", focused: true });
    recorder.record({ type: "gesture-move", focused: true });
    recorder.record({ type: "gesture-cancel", focused: false, reason: "window-blur" });
    const report = recorder.finish()!;
    assert.equal(report.result, "gesture-cancelled");
    assert.equal(report.events.at(-1)?.reason, "window-blur");
});

test("port recording accepts normal release and resets between attempts", () => {
    const recorder = new PortGrabRecorder();
    recorder.start("test");
    recorder.record({ type: "pointerdown", focused: true, button: 0, port: "out" });
    recorder.record({ type: "gesture-start", focused: true });
    recorder.record({ type: "gesture-move", focused: true });
    recorder.record({ type: "gesture-cancel", focused: true, reason: "release" });
    assert.equal(recorder.finish()?.result, "grab-observed");
    assert.equal(recorder.active, false);
    assert.equal(recorder.finish(), null);
    recorder.start("next");
    assert.equal(recorder.finish()?.result, "no-pointer-attempt-observed");
    recorder.start("next");
    recorder.record({ type: "pointerdown", focused: true, button: 0 });
    assert.equal(recorder.finish()?.result, "press-not-on-port");
    recorder.start("next");
    recorder.record({ type: "pointerdown", focused: true, button: 0, port: "out" });
    recorder.record({ type: "gesture-start", focused: true });
    assert.equal(recorder.finish()?.result, "no-drag-motion");
});

test("port recording bounds storage while retaining the initial press and final events", () => {
    let now = 10;
    const recorder = new PortGrabRecorder(() => now);
    recorder.start("test");
    recorder.record({ type: "pointerdown", focused: true, button: 0, port: "out" });
    for (let index = 0; index < 600; ++index) {
        now++;
        recorder.record({ type: "pointermove", focused: true, buttons: 1, x: index, target: `port-${index}` });
    }
    recorder.record({ type: "pointerup", focused: true, button: 0 });
    const report = recorder.finish()!;
    assert.equal(report.truncated, true);
    assert.equal(report.events.length, 256);
    assert.equal(report.events[0].type, "pointerdown");
    assert.equal(report.events[0].elapsedMs, 0);
    assert.equal(report.events.at(-1)?.type, "pointerup");
    assert.equal(report.events.at(-1)?.elapsedMs, 600);
});

test("port recording drops repetitive motion but retains capture and button changes", () => {
    const recorder = new PortGrabRecorder();
    recorder.start("test");
    const move = { type: "pointermove", focused: true, buttons: 1, capture: true, gesture: true };
    for (let x = 0; x < 100; ++x) recorder.record({ ...move, x });
    recorder.record({ ...move, capture: false });
    recorder.record({ ...move, capture: false, buttons: 0 });
    assert.equal(recorder.finish()?.events.length, 3);
});

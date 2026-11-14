import test from "node:test";
import assert from "node:assert/strict";
import { clientToServerClock, knobAudioRecord, recordingPathFor, KnobPointerTrace } from "../src/knobAudioRecording";

test("gesture times map onto the server steady clock around the capture start request", () => {
    const clock = clientToServerClock(100, 104, 5_000_000);
    assert.equal(clock.uncertaintyUs, 2000);
    assert.equal(clock.toServerUs(102), 5_000_000);
    assert.equal(clock.toServerUs(112.5), 5_010_500);
    const record = knobAudioRecord({
        recording: false, path: "/tmp/iv-knob-audio-x.wav", sampleRate: 48000, channels: 2, sampleCount: 1024,
        blocks: [[0, 256, 5_001_000], [512, 256, 5_006_333]], serverTimeMs: 0, serverSteadyUs: 5_020_000,
    }, [{ kind: "update", interactionId: "drag", inputName: "frequency", value: 220, clientMs: 103.0004 }], clock);
    assert.deepEqual(record.blocks, [{ offset: 0, frames: 256, steadyUs: 5_001_000 }, { offset: 512, frames: 256, steadyUs: 5_006_333 }]);
    assert.deepEqual(record.gestures, [{ kind: "update", interactionId: "drag", inputName: "frequency", value: 220, steadyUs: 5_001_000 }]);
    assert.equal(recordingPathFor("/tmp/iv-knob-audio-x.wav"), "/tmp/iv-knob-audio-x.json");
});

test("pointer trace retains rapid press/release transitions and bounds movement collection", () => {
    const trace = new KnobPointerTrace(5);
    let measured = 0;
    const detail = () => ({ target: ++measured });
    trace.record("press", 100, detail);
    trace.record("move", 101, detail);
    trace.record("move", 102, detail); // Throttled before layout measurement.
    trace.record("release", 103, detail);
    trace.record("press", 104, detail);
    trace.record("lock-change", 105, detail);
    trace.record("move", 135, detail); // Bounded before layout measurement.
    assert.equal(measured, 5);
    assert.deepEqual(trace.snapshot().events.map((event) => event.kind),
        ["press", "move", "release", "press", "lock-change"]);
    assert.equal(trace.snapshot().dropped, 1);
});

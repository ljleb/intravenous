import test from "node:test";
import assert from "node:assert/strict";
import { audioOutputDiagnostic } from "../src/audioOutputDiagnostic";

test("startup audio diagnostics distinguish hardware output from the silent fallback", () => {
    assert.equal(audioOutputDiagnostic({ deviceId: "default", name: "System Default", available: true, error: null }),
        "Audio output: System Default [default]; active");
    assert.equal(audioOutputDiagnostic({ deviceId: null, name: null, available: false, error: null }),
        "Audio output: disabled; Tick uses the silent fallback clock");
    assert.equal(audioOutputDiagnostic({ deviceId: "default", name: null, available: false, error: "failed to open playback device" }),
        "Audio output: default [default]; unavailable; Tick uses the silent fallback clock; failed to open playback device");
});

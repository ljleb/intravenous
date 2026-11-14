import test from "node:test";
import assert from "node:assert/strict";
import { ControlCompilationProgress, type ControlCompileStatus } from "../src/controlCompilation";

function status(requestId: number, phase: ControlCompileStatus["phase"], changes: Partial<ControlCompileStatus> = {}): ControlCompileStatus {
    return { requestId, phase, projectGeneration: 3, dynamic: true, predictive: true,
        inputs: ["frequency"], elapsedUs: 15000, queueUs: 2000, preparationUs: 14000,
        stageUs: 50, message: "", ...changes };
}

test("control compile progress counts pending work and clears superseded and failed requests", () => {
    const progress = new ControlCompilationProgress();
    let value = progress.receive(status(1, "queued"));
    assert.equal(value.queued, 1);
    assert.match(value.text, /predictive preparation/);
    value = progress.receive(status(1, "compiling"));
    assert.equal(value.compiling, 1);
    assert.equal(value.queued, 0);
    progress.receive(status(2, "queued"));
    value = progress.receive(status(2, "superseded"));
    assert.equal(value.queued, 0);
    assert.equal(value.compiling, 1);
    value = progress.receive(status(1, "failed", { message: "invalid callback" }));
    assert.equal(value.compiling, 0);
    assert.equal(value.failed, true);
    assert.match(value.text, /invalid callback/);
});

test("prepared hover code and later staging report distinct measured endpoints", () => {
    const progress = new ControlCompilationProgress();
    const prepared = progress.receive(status(1, "prepared"));
    assert.match(prepared.text, /request-to-ready 14\.00 ms/);
    const staged = progress.receive(status(1, "staged", { elapsedUs: 2_000_000 }));
    assert.match(staged.text, /staging 0\.05 ms/);
    assert.match(staged.text, /request-to-staging 2000\.00 ms/);
    assert.doesNotMatch(staged.text, /activated/);
    assert.equal(staged.compiling, 0);
    const storage = progress.receive(status(1, "compilingStorage"));
    assert.equal(storage.compiling, 1);
    assert.match(storage.text, /storage transition/);
    progress.clear();
    const afterReconnect = progress.receive(status(2, "queued", { dynamic: false, inputs: [] }));
    assert.equal(afterReconnect.compiling, 0);
    assert.equal(afterReconnect.queued, 1);
    assert.match(afterReconnect.text, /settled/);
});

import assert from "node:assert/strict";
import test from "node:test";
import { ClientInteractionRecorder, compareInteractionSends, ObservationTraceInput } from "../src/clientInteractionTrace";
import { answerObservationRequest } from "../src/observationEndpoint";
import { PresentationObservations, type ObservationScheduler } from "../src/presentationObservations";
import type { ObservationHostRequest } from "../src/observations";

test("a replay preserves an in-flight reconciliation and replaces obsolete pending interest", async () => {
    const timers = new Map<() => void, number>();
    const scheduler: ObservationScheduler = {
        after(ms, run) { timers.set(run, ms); return () => { timers.delete(run); }; },
        every() { return () => {}; },
    };
    const flush = async () => { for (let i = 0; i < 12; ++i) await Promise.resolve(); };
    const nextFrame = async () => {
        for (const [run, ms] of timers) if (ms === 16) { timers.delete(run); run(); }
        await flush();
    };
    const reconcile: Array<Extract<ObservationHostRequest, { type: "observationReconcile" }>> = [];
    const observations = new PresentationObservations((request) => {
        if (request.type === "observationReconcile") reconcile.push(request);
    }, undefined, undefined, scheduler);
    const input = new ObservationTraceInput(observations);
    const record = new ClientInteractionRecorder(observations);
    try {
        const viewport = (begin: string) => input.apply({
            type: "port", id: "beat-grid", scheduling: "viewport",
            observation: { source: { node: { instanceId: "timeline" }, kind: "input", member: "beats", portKind: "event" },
                operation: "beat-grid", window: { begin, length: "32", points: 16 } },
        });
        const reply = (index: number) => observations.receive({ type: "observationResponse",
            requestId: reconcile[index].requestId, result: { revision: String(index + 1) } });
        viewport("0");
        await nextFrame();
        viewport("32");
        await nextFrame();
        viewport("64");
        await nextFrame();
        assert.deepEqual(reconcile.map((request) => request.interests[0].window.begin), ["0"]);
        reply(0);
        await flush();
        assert.deepEqual(reconcile.map((request) => request.interests[0].window.begin), ["0", "64"]);
        reply(1);
        await flush();
        assert.deepEqual(record.stop().events.map(({ value }) => value.type === "observation" && value.input.type === "port"
            ? value.input.observation.window.begin : null), ["0", "32", "64"]);
    } finally {
        observations.stopRecording();
        observations.dispose();
    }
});

test("observation requests use the same visibility and prediction rules in either host", async () => {
    const calls: unknown[][] = [];
    const transport = {
        describeObservations: async () => ({ capabilities: [] }),
        reconcileObservations: async (...args: unknown[]) => { calls.push(args); return { revision: "2" }; },
    };
    const interest = { id: "meter", operation: "level", source: { node: { instanceId: "saw" }, kind: "output" as const, member: "value" } };
    const request = { type: "observationReconcile" as const, requestId: "window", interests: [interest], predictedInterests: [interest] };
    for (const view of [{ visible: true, active: true }, { visible: true, active: false }, { visible: false, active: true }])
        assert.equal((await answerObservationRequest(transport, view, "owner", request)).requestId, "window");
    assert.deepEqual(calls, [["owner", [interest], [interest]], ["owner", [interest], undefined], ["owner", [], undefined]]);
});

test("request comparison normalizes ownership while retaining the difference between an update and a request", () => {
    const update = (owner: string, notification: boolean) => [{ atMs: 1, value: {
        method: "manualControls.update", params: { interactionId: JSON.stringify([owner, "gesture"]), value: 3 }, notification,
    } }];
    assert.equal(compareInteractionSends(update("vscode", true), "vscode", update("cli", true), "cli").difference, null);
    assert.equal(compareInteractionSends(update("vscode", true), "vscode", update("cli", false), "cli").matchingPrefix, 0);
});

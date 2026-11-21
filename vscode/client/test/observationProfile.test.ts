import assert from "node:assert/strict";
import test from "node:test";
import { ObservationProfile, observationInterestReport } from "../src/observationProfile";
import type { ObservationDelivery } from "../src/observationDelivery";
import { ObservationValues, type ObservationInterest } from "../src/observations";

const body = { regions: [{ begin: "0", length: "64", points: [{ index: "0", available: true, value: { value: .5 } }] }] };
const initial: ObservationDelivery = {
    snapshot: true, activationId: "1", generation: "2", revision: "3", index: "0", blockSize: 64, dataVersion: "1",
    scopes: { viewport: { values: { meter: { available: true, value: { dB: -12 } } }, windows: {
        min: { window: { begin: "0", length: "64", points: 8 }, status: "ready", approximate: true, body },
    } } },
};

test("observation profiling separates transmitted samples from status-only and metadata-only patches", () => {
    const profile = new ObservationProfile();
    const updates = [
        initial,
        { ...initial, snapshot: false, activationId: "2", scopes: { viewport: { windows: { min: { outdated: true } } } } },
        { ...initial, snapshot: false, scopes: {} },
        { ...initial, snapshot: false, scopes: { viewport: null } },
    ];
    const messages = updates.map((update) => ({ type: "observationUpdate" as const, update }));
    for (const message of messages) profile.record(message, message.update.activationId === "1");
    const result = profile.summary();
    assert.equal(result.updates, 4);
    assert.equal(result.presentationUpdates, 3);
    assert.equal(result.snapshots, 1);
    assert.equal(result.messageBytes, messages.reduce((n, message) => n + Buffer.byteLength(JSON.stringify(message)), 0));
    assert.equal(result.statusChanges, 2);
    assert.equal(result.liveValues, 1);
    assert.equal(result.windowChanges, 2);
    assert.equal(result.windowBodies, 1);
    assert.equal(result.windowBytes, Buffer.byteLength(JSON.stringify(body)));
    assert.equal(result.metadataOnly, 1);
    assert.equal(result.removals, 1);
});

test("observation profiling links lane interests to replies and reports retained data separately", () => {
    const profile = new ObservationProfile();
    const values = new ObservationValues();
    const window = { begin: "0", length: "64", points: 8 };
    const min: ObservationInterest = { id: "min", source: { node: { instanceId: "curve" }, kind: "input", member: "min" },
        operation: "waveform", window };
    const meter: ObservationInterest = { id: "meter", source: { node: { instanceId: "curve" }, kind: "output", member: "value" },
        operation: "level" };
    const label = (interest: ObservationInterest) => ({ consumer: "Automation Curve", source: interest.source.member });
    profile.recordRequests([min], label, true);
    profile.recordRequests([min, meter], label);
    profile.recordBudgetSkip({ ...min, id: "prefetch", prefetch: true }, label);
    values.expectWindows([min, meter]);
    values.receive(initial);
    profile.record({ type: "observationUpdate", update: {
        ...initial, snapshot: false, scopes: { viewport: {
            values: { meter: { available: true, value: { dB: -6 } } },
            windows: { min: { outdated: true } },
        } },
    } }, true);
    const summary = profile.summary(values);
    const [active, live, skipped] = summary.interests;
    assert.equal(summary.reconcileRequests, 1);
    assert.equal(active.atStart, true);
    assert.equal(active.cached?.status, "ready");
    assert.equal(active.bodies, 0);
    assert.equal(active.outdated, 1);
    assert.equal(live.live.available, 1);
    assert.equal(skipped.requests, 0);
    assert.equal(skipped.budgetSkips, 1);
    assert.match(observationInterestReport(summary).join("\n"), /Automation Curve\n[\s\S]*Cached at finish ready/);
});

test("profile records viewport, submission and first usable result for each arrival", () => {
    let now = 0;
    const profile = new ObservationProfile(() => now);
    const values = new ObservationValues();
    const meter = { id: "meter", source: { node: { instanceId: "curve" }, kind: "output" as const, member: "value" }, operation: "level" };
    const label = () => ({ consumer: "Curve", source: "value", eligibilityKey: "curve" });
    const update = { ...initial, snapshot: false };
    const usable = (id: string) => profile.recordUsable(id);
    profile.recordRequests([meter], label, true);
    values.receive(initial, usable);
    assert.equal(profile.summary().interests[0].firstResult, undefined);
    now = 10;
    profile.recordEligibility("curve", true);
    now = 35;
    profile.recordRequests([meter], label);
    now = 45;
    values.receive({ ...update, scopes: { viewport: { values: { meter: { available: false, pending: true } } } } }, usable);
    assert.equal(profile.summary().interests[0].firstResult?.submitToUsableMs, undefined);
    now = 70;
    values.receive({ ...update, scopes: initial.scopes }, usable);
    assert.deepEqual(profile.summary().interests[0].firstResult, { eligibilityToSubmitMs: 25, submitToUsableMs: 35 });
    now = 80;
    profile.recordEligibility("curve", false);
    now = 90;
    profile.recordEligibility("curve", true);
    now = 95;
    profile.recordRequests([meter], label);
    assert.deepEqual(profile.summary().interests[0].firstResult, { eligibilityToSubmitMs: 5, submitToUsableMs: undefined });
});

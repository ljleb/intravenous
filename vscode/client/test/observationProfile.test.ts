import assert from "node:assert/strict";
import test from "node:test";
import { ObservationProfile } from "../src/observationProfile";
import type { ObservationDelivery } from "../src/observationDelivery";

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

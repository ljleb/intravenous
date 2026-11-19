import assert from "node:assert/strict";
import test from "node:test";
import { ObservationProfile } from "../src/observationProfile";
import type { ObservationUpdate, ObservationWindowResult } from "../src/observations";

const result: ObservationWindowResult = {
    id: "min", window: { begin: "0", length: "64", points: 8 },
    status: "ready", approximate: true, dataVersion: "1",
    regions: [{ begin: "0", length: "64", points: [{ index: "0", available: true, value: { value: .5 } }] }],
};

function batch(window: ObservationWindowResult, overrides: Partial<ObservationUpdate> = {}) {
    return {
        type: "observationUpdate" as const,
        update: {
            activationId: "1", generation: "2", revision: "3", index: "0", blockSize: 64, dataVersion: "1",
            scopes: [{ scopeId: "viewport", values: [{ id: "meter", available: true, value: { dB: -12 } }], windows: [window] }],
            ...overrides,
        },
    };
}

test("observation profiling counts resent windows even when a seek changes the activation", () => {
    const profile = new ObservationProfile();
    const messages = [
        batch(result),
        batch(result, { index: "64", dataVersion: "2" }),
        batch({ ...result, dataVersion: "3", outdated: true }, { activationId: "2", dataVersion: "3" }),
        batch({ ...result, status: "pending", regions: [] }),
    ];
    for (const message of messages) profile.record(message, message.update.activationId === "1");

    const s = profile.summary();
    assert.equal(s.updates, 4);
    assert.equal(s.presentationUpdates, 3);
    assert.equal(s.messageBytes, messages.reduce((total, message) => total + Buffer.byteLength(JSON.stringify(message)), 0));
    assert.equal(s.unchangedScopes, 1);
    assert.equal(s.unchangedScopesWithNewDataVersion, 1);
    assert.equal(s.repeatedLiveValues, 3);
    assert.equal(s.readyWindows, 3);
    assert.equal(s.outdatedWindows, 1);
    assert.equal(s.pendingWindows, 1);
    assert.equal(s.repeatedWindows, 2);
    assert.equal(s.repeatedWindowsWithNewDataVersion, 1);
    assert.equal(s.repeatedWindowBytes, s.windowBytes * 2 / 3);
});

test("observation profiling separates different requested windows and graph generations", () => {
    const profile = new ObservationProfile();
    profile.record(batch(result), true);
    const shifted = { ...result, window: { ...result.window, begin: "16" } };
    profile.record(batch(shifted), true);
    profile.record(batch(shifted, { generation: "3" }), true);

    const s = profile.summary();
    assert.equal(s.readyWindows, 3);
    assert.equal(s.repeatedWindows, 0);
    assert.equal(s.repeatedLiveValues, 1);
    assert.equal(s.unchangedScopes, 0);
});

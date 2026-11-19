import assert from "node:assert/strict";
import test from "node:test";
import { ObservationDeliveryReceiver, ObservationDeliveryState, type ObservationDelivery } from "../src/observationDelivery";

const body = { regions: [], marks: [{ index: "0", depth: 1 }], unavailableRegions: [{ begin: "32", length: "32" }] };
const initial: ObservationDelivery = {
    snapshot: true, activationId: "1", generation: "2", revision: "3", index: "0", blockSize: 64, dataVersion: "1",
    scopes: { viewport: { values: { meter: { available: true, value: { peak: .5 } } }, windows: {
        beats: { window: { begin: "0", length: "64", points: 8 }, status: "ready", approximate: false, dataVersion: "1", body },
    } } },
};

test("status-only patches keep the window data and removals clear individual readings", () => {
    const state = new ObservationDeliveryState();
    state.receive(initial);
    state.receive({ ...initial, snapshot: false, dataVersion: "2", scopes: { viewport: {
        values: { meter: null }, windows: { beats: { outdated: true } },
    } } });
    const scope = state.read("viewport")!.scopes[0];
    assert.equal(scope.windows![0].outdated, true);
    assert.equal(scope.windows![0].marks, body.marks);
    assert.deepEqual(scope.values, []);
    state.receive({ ...initial, snapshot: false, scopes: { viewport: {
        windows: { beats: { status: "pending", outdated: null, body: null } },
    } } });
    assert.equal(state.read("viewport")!.scopes[0].windows![0].outdated, undefined);
    assert.equal(state.read("viewport")!.scopes[0].windows![0].marks, undefined);
    state.receive({ ...initial, snapshot: false, scopes: { viewport: null } });
    assert.deepEqual(state.read("viewport")!.scopes[0].windows, []);
});

test("a later snapshot repairs missed patches and replays the current panel state", () => {
    const host = new ObservationDeliveryState();
    host.receive(initial);
    const view = new ObservationDeliveryReceiver();
    assert.equal(view.receive(host.snapshot("viewport", "viewport")!, 1), "applied");
    const delta: ObservationDelivery = { ...initial, snapshot: false, scopes: {
        viewport: { windows: { beats: { status: "ready", dataVersion: "3" } } },
    } };
    host.receive(delta);
    assert.equal(view.receive(delta, 3, 2), "resync");
    assert.equal(view.receive(delta, 4, 3), "ignored");
    assert.equal(view.read("viewport")!.scopes[0].windows![0].dataVersion, "1");
    assert.equal(view.receive(host.snapshot("viewport", "viewport")!, 5), "applied");
    assert.equal(view.read("viewport")!.scopes[0].windows![0].dataVersion, "3");
    assert.equal(view.receive(delta, 4, 3), "ignored");
    const empty = { ...initial, generation: "3", scopes: {} };
    host.receive(empty);
    assert.deepEqual(host.snapshot("viewport", "viewport")!.scopes, { viewport: {} });
    host.clear();
    assert.equal(host.snapshot("viewport", "viewport"), undefined);
});

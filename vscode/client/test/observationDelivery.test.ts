import assert from "node:assert/strict";
import test from "node:test";
import { effect, shallowReactive, stop } from "vue";
import { ObservationDeliverySequence, ObservationDeliveryState, type ObservationDelivery } from "../src/observationDelivery";
import { ObservationValues } from "../src/observations";

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
    const scope = state.changes("viewport");
    assert.equal(scope.windows[0][1]?.outdated, true);
    assert.equal(scope.windows[0][1]?.marks, body.marks);
    assert.deepEqual(scope.values, []);
    state.receive({ ...initial, snapshot: false, scopes: { viewport: {
        windows: { beats: { status: "pending", outdated: null, body: null } },
    } } });
    assert.equal(state.changes("viewport").windows[0][1]?.outdated, undefined);
    assert.equal(state.changes("viewport").windows[0][1]?.marks, undefined);
    state.receive({ ...initial, snapshot: false, scopes: { viewport: null } });
    assert.deepEqual(state.changes("viewport").windows, []);
});

test("a later snapshot repairs missed patches and replays the current panel state", () => {
    const host = new ObservationDeliveryState();
    host.receive(initial);
    const sequence = new ObservationDeliverySequence();
    const view = new ObservationDeliveryState();
    const receive = (update: ObservationDelivery, next: number, previous?: number) => {
        const result = sequence.receive(update, next, previous);
        if (result === "applied") view.receive(update);
        return result;
    };
    assert.equal(receive(host.snapshot("viewport", "viewport")!, 1), "applied");
    const delta: ObservationDelivery = { ...initial, snapshot: false, scopes: {
        viewport: { windows: { beats: { status: "ready", dataVersion: "3" } } },
    } };
    host.receive(delta);
    assert.equal(receive(delta, 3, 2), "resync");
    assert.equal(receive(delta, 4, 3), "ignored");
    assert.equal(view.changes("viewport").windows[0][1]?.dataVersion, "1");
    assert.equal(receive(host.snapshot("viewport", "viewport")!, 5), "applied");
    assert.equal(view.changes("viewport").windows[0][1]?.dataVersion, "3");
    assert.equal(receive(delta, 4, 3), "ignored");
    const empty = { ...initial, generation: "3", scopes: {} };
    host.receive(empty);
    assert.deepEqual(host.snapshot("viewport", "viewport")!.scopes, { viewport: {} });
    host.clear();
    assert.equal(host.snapshot("viewport", "viewport"), undefined);
});

test("a live patch leaves unrelated window readers alone", () => {
    const values = new ObservationValues("viewport", shallowReactive);
    values.expectWindows([{ id: "beats", source: { node: { instanceId: "timeline" }, kind: "input", member: "beats" },
        operation: "beat-grid", window: { begin: "0", length: "64", points: 8 } }]);
    values.receive(initial);
    let reads = 0;
    const reader = effect(() => { values.windowForDisplay("beats"); ++reads; });
    try {
        const delta = { ...initial, snapshot: false };
        assert.equal(values.receive({ ...delta, scopes: {} }), false);
        assert.equal(values.receive({ ...delta, scopes: { viewport: { values: { meter: null } } } }), true);
        assert.equal(reads, 1);
        assert.equal(values.receive({ ...delta, scopes: { viewport: { windows: { beats: { outdated: true } } } } }), true);
        assert.equal(reads, 2);
        assert.equal(values.windowForDisplay("beats")?.marks, body.marks);
    } finally { stop(reader); }
});

test("the next accepted scope includes patches received while waiting for its generation and revision", () => {
    const values = new ObservationValues();
    const window = { begin: "0", length: "64", points: 8 };
    values.expectWindows([{ id: "beats", source: { node: { instanceId: "timeline" }, kind: "input", member: "beats" },
        operation: "beat-grid", window }]);
    values.acceptGeneration(1);
    assert.equal(values.receive(initial), false);
    assert.equal(values.receive({ ...initial, snapshot: false, scopes: {
        viewport: { windows: { beats: { dataVersion: "2" } } },
    } }), false);
    values.acceptGeneration(2);
    assert.equal(values.values.get("meter")?.value?.peak, .5);
    assert.equal(values.windowForDisplay("beats")?.dataVersion, "2");

    values.acceptRevision("4");
    assert.equal(values.receive({ ...initial, snapshot: false, scopes: {
        viewport: { values: { meter: { available: true, value: { peak: .75 } } } },
    } }), false);
    assert.equal(values.values.get("meter")?.value?.peak, .5);
    assert.equal(values.receive({ ...initial, snapshot: false, revision: "4", scopes: {} }), true);
    assert.equal(values.values.get("meter")?.value?.peak, .75);
    assert.equal(values.windowForDisplay("beats")?.marks, body.marks);
});

test("returning to a requested range reuses the latest matching window without another patch", () => {
    const values = new ObservationValues();
    const interest = { id: "beats", source: { node: { instanceId: "timeline" }, kind: "input" as const, member: "beats" },
        operation: "beat-grid", window: { begin: "0", length: "64", points: 8 } };
    values.expectWindows([interest]);
    values.receive(initial);
    values.expectWindows([{ ...interest, window: { ...interest.window, begin: "128" } }]);
    assert.equal(values.receive({ ...initial, snapshot: false, scopes: {
        viewport: { windows: { beats: { dataVersion: "2" } } },
    } }), false);
    values.expectWindows([interest]);
    assert.equal(values.windowForDisplay("beats")?.dataVersion, "2");
    assert.equal(values.windowForDisplay("beats")?.marks, body.marks);
});

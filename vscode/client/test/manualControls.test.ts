import test from "node:test";
import assert from "node:assert/strict";
import { ManualControlInteractions, type ManualControlDescriptor, type ManualControlId,
    type ManualControlTransport } from "../src/manualControls";
import { ManualControlGestures } from "../src/manualControlGesture";
import type { GraphScope } from "../src/graphScope";

const control: ManualControlId = { node: { instanceId: "osc" }, inputName: "frequency" };
const descriptor: ManualControlDescriptor = { value: 440, enabled: true,
    participation: "disconnectedOnly", available: true, participates: true };
function scope(available = true, value = 440): GraphScope {
    return { generation: 1, editable: true, connections: [], nodes: [{
        address: control.node, loadState: "ready", instance: { instanceId: "osc", definitionId: "oscillator", realized: true },
        sampleInputs: [{ name: "frequency", index: 0, manualControl: { ...descriptor, available, value } }],
        sampleOutputs: [], eventInputs: [], eventOutputs: [], hasChildren: false,
    }] };
}
function deferred() {
    let resolve!: () => void;
    const promise = new Promise<void>((done) => { resolve = done; });
    return { promise, resolve };
}
function harness(overrides: Partial<ManualControlTransport> = {}) {
    const calls: Array<{ action: string; id?: string; value?: number }> = [];
    const transport: ManualControlTransport = {
        hoverManualControl: async () => { calls.push({ action: "hover" }); },
        beginManualControl: async (id, _control, value) => { calls.push({ action: "begin", id, value }); },
        updateManualControl: async (id, value) => { calls.push({ action: "update", id, value }); },
        endManualControl: async (id, value) => { calls.push({ action: "end", id, value }); },
        ...overrides,
    };
    const manager = new ManualControlInteractions(() => transport, () => {});
    manager.reconcile(scope());
    return { manager, calls };
}

test("manual gestures coalesce while begin is pending and finish with the latest value", async () => {
    const begin = deferred();
    const { manager, calls } = harness({ beginManualControl: async (_id, _control, value) => {
        calls.push({ action: "begin", value }); await begin.promise;
    } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 440 });
    await Promise.resolve();
    for (let value = 441; value < 500; ++value)
        manager.action("panel", { action: "update", interactionId: "drag", value });
    manager.action("panel", { action: "end", interactionId: "drag", value: 500 });
    assert.equal(calls.length, 1);
    begin.resolve(); await manager.settled();
    assert.deepEqual(calls.map(({ action, value }) => ({ action, value })), [
        { action: "begin", value: 440 }, { action: "end", value: 500 },
    ]);
    assert.equal(manager.states("panel")[0].phase, "settled");
    assert.equal(manager.states("panel")[0].value, 500);
    manager.reconcile(scope(true, 500));
    assert.equal(manager.states("panel")[0].value, 500);
    manager.reconcile(scope(true, 440)); // Undo from another client.
    assert.equal(manager.states("panel")[0].value, 440);
});

test("in-flight manual updates keep one latest pending update", async () => {
    const update = deferred();
    const { manager, calls } = harness({ updateManualControl: async (_id, value) => {
        calls.push({ action: "update", value }); await update.promise;
    } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 440 });
    await manager.settled();
    manager.action("panel", { action: "update", interactionId: "drag", value: 450 });
    await Promise.resolve();
    for (let value = 451; value < 500; ++value)
        manager.action("panel", { action: "update", interactionId: "drag", value });
    update.resolve(); await manager.settled();
    assert.deepEqual(calls.filter((entry) => entry.action === "update").map((entry) => entry.value), [450, 499]);
    manager.action("panel", { action: "end", interactionId: "drag", value: 499 });
    await manager.settled();
});

test("compatible scope refresh keeps a gesture and panel close ends it", async () => {
    const { manager, calls } = harness();
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.reconcile({ ...scope(true, 450), generation: 2 });
    assert.equal(manager.states("panel")[0].phase, "active");
    manager.releaseOwner("panel"); await manager.settled();
    assert.deepEqual(calls.map((entry) => entry.action), ["begin", "end"]);
    assert.deepEqual(manager.states("panel"), []);
});

test("target removal ends a gesture explicitly and ignores late pointer events", async () => {
    const { manager, calls } = harness();
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.reconcile({ ...scope(), nodes: [] }); await manager.settled();
    manager.action("panel", { action: "update", interactionId: "drag", value: 900 });
    manager.action("panel", { action: "end", interactionId: "drag", value: 900 });
    await manager.settled();
    assert.deepEqual(calls.map((entry) => [entry.action, entry.value]), [["begin", 450], ["end", 450]]);
    assert.match(manager.states("panel")[0].error!, /removed or became incompatible/);
});

test("rejected manual updates close at the last accepted value without retrying", async () => {
    const { manager, calls } = harness({ updateManualControl: async () => { throw new Error("stale project revision"); } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.action("panel", { action: "update", interactionId: "drag", value: 900 });
    await manager.settled();
    assert.deepEqual(calls.map((entry) => [entry.action, entry.value]), [["begin", 450], ["end", 450]]);
    assert.equal(manager.states("panel")[0].error, "stale project revision");
});

test("disconnect prevents queued gestures and late begin replies from reaching a replacement session", async () => {
    const begin = deferred();
    const { manager, calls } = harness({ beginManualControl: async () => { calls.push({ action: "begin" }); await begin.promise; } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await Promise.resolve();
    manager.action("panel", { action: "end", interactionId: "drag", value: 500 });
    manager.disconnect(); manager.reconcile(scope());
    begin.resolve(); await manager.settled(); await Promise.resolve();
    assert.deepEqual(calls, [{ action: "begin" }]);
    assert.deepEqual(manager.states("panel"), []);
});

test("two panels cannot reuse one another's interaction IDs or end one another's gesture", async () => {
    const { manager, calls } = harness();
    manager.action("a", { action: "begin", interactionId: "same", control, value: 450 });
    await manager.settled();
    manager.action("b", { action: "end", interactionId: "same", value: 900 });
    assert.throws(() => manager.action("b", { action: "begin", interactionId: "same", control, value: 900 }), /already has an active/);
    await manager.settled();
    assert.deepEqual(calls.map((entry) => entry.action), ["begin"]);
    manager.releaseOwner("a"); await manager.settled();
});

test("presentation remount does not restart or end a pointer gesture", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "gesture", () => {});
    gestures.scrub(control, descriptor, 7, 100, 2);
    // A newly mounted component binds to the same host, regardless of display mode.
    assert.equal(gestures.state(control)?.value, 440);
    gestures.move(7, 120, 20, 20000);
    gestures.receive([{ interactionId: "gesture", control, value: 440, phase: "active" }]);
    assert.equal(gestures.state(control)?.value, 480); // Old acknowledgement cannot rewind the pointer.
    gestures.edit(control, descriptor); // A remounted input resumes the same gesture.
    gestures.release(7);
    assert.deepEqual(actions, [
        { action: "begin", interactionId: "gesture", control, value: 440 },
        { action: "update", interactionId: "gesture", value: 480 },
        { action: "end", interactionId: "gesture", value: 480 },
    ]);
});

test("manual pointer tracking clamps bounds and ignores other pointers", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "gesture", () => {});
    gestures.scrub(control, descriptor, 7, 100, 2);
    gestures.move(8, 120, 20, 500);
    assert.equal(actions.length, 1);
    gestures.move(7, 1000, 20, 500);
    assert.equal(gestures.state(control)?.value, 500);
    gestures.release(8);
    assert.equal(actions.length, 2);
    gestures.release(7);
    assert.equal(actions.length, 3);
});

test("host rejection invalidates the webview gesture before late pointer movement", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "gesture", () => {});
    gestures.scrub(control, descriptor, 7, 100, 2);
    gestures.receive([{ interactionId: "gesture", control, value: 440, phase: "failed", error: "input unavailable" }]);
    gestures.move(7, 120);
    gestures.release(7);
    assert.equal(actions.length, 1);
    assert.equal(gestures.state(control)?.error, "input unavailable");
});

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
        updateManualControl: (id, value) => { calls.push({ action: "update", id, value }); },
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

test("an acknowledged gesture streams every update at once, even behind a pending reply", async () => {
    const hover = deferred();
    const { manager, calls } = harness({ hoverManualControl: async () => { calls.push({ action: "hover" }); await hover.promise; } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 440 });
    await manager.settled();
    // A slow ordered request must not hold back live values.
    manager.action("panel", { action: "hover", control: { ...control, inputName: "other" } });
    for (let value = 450; value < 455; ++value)
        manager.action("panel", { action: "update", interactionId: "drag", value });
    manager.action("panel", { action: "update", interactionId: "drag", value: 454 }); // Unchanged.
    assert.deepEqual(calls.filter((entry) => entry.action === "update").map((entry) => entry.value), [450, 451, 452, 453, 454]);
    manager.action("panel", { action: "end", interactionId: "drag", value: 454 });
    hover.resolve(); await manager.settled();
    assert.deepEqual(calls.at(-1), { action: "end", id: calls.find((entry) => entry.action === "begin")!.id, value: 454 });
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

test("an update that cannot be sent closes the gesture at the last sent value without retrying", async () => {
    const { manager, calls } = harness({ updateManualControl: () => { throw new Error("client is not connected"); } });
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.action("panel", { action: "update", interactionId: "drag", value: 900 });
    await manager.settled();
    assert.deepEqual(calls.map((entry) => [entry.action, entry.value]), [["begin", 450], ["end", 450]]);
    assert.equal(manager.states("panel")[0].error, "client is not connected");
});

test("other views and clients show live gesture values until a read reports the gesture closed", async () => {
    const { manager } = harness();
    const live = (owner: string) => manager.live(owner).map((entry) => [entry.control.inputName, entry.value]);
    manager.action("a", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.action("a", { action: "update", interactionId: "drag", value: 460 });
    assert.deepEqual(live("a"), []); // The owner shows its own gesture state.
    assert.deepEqual(live("b"), [["frequency", 460]]);
    const remote = { node: { instanceId: "osc" }, inputName: "detune" };
    manager.receiveLive({ control: remote, value: 2 });
    assert.deepEqual(live("b"), [["detune", 2], ["frequency", 460]]);
    const withDetune = (detune: Partial<ManualControlDescriptor>): GraphScope => {
        const value = scope(true, 450);
        value.nodes[0].sampleInputs.push({ name: "detune", index: 1, manualControl: { ...descriptor, value: 0, ...detune } });
        return value;
    };
    manager.reconcile(withDetune({ live: true, liveValue: 1 }));
    assert.deepEqual(live("b"), [["detune", 2], ["frequency", 460]]); // A newer relayed value wins.
    manager.reconcile(withDetune({ live: false }));
    assert.deepEqual(live("b"), [["frequency", 460]]);
    manager.reconcile(withDetune({ live: true, liveValue: 3 })); // A view opened mid-gesture.
    assert.deepEqual(live("b"), [["detune", 3], ["frequency", 460]]);
    manager.disconnect();
    assert.deepEqual(live("b"), []);
});

test("the recorder observer sees each gesture message in sending order", async () => {
    const { manager } = harness();
    const events: Array<[string, number]> = [];
    manager.observe((event) => events.push([event.kind, event.value]));
    manager.action("panel", { action: "begin", interactionId: "drag", control, value: 450 });
    await manager.settled();
    manager.action("panel", { action: "update", interactionId: "drag", value: 451 });
    manager.action("panel", { action: "end", interactionId: "drag", value: 452 });
    await manager.settled();
    manager.observe(null);
    manager.action("panel", { action: "begin", interactionId: "again", control, value: 452 });
    await manager.settled();
    assert.deepEqual(events, [["begin", 450], ["update", 451], ["end", 452]]);
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

test("a held knob drag ignores another knob's hover and press until release", () => {
    const actions: unknown[] = [];
    let next = 0;
    const gestures = new ManualControlGestures((action) => actions.push(action), () => `drag-${++next}`, () => {});
    const other = { ...control, inputName: "detune" };
    gestures.hover(control, descriptor);
    assert.equal(gestures.scrub(control, descriptor, 1, 100, 1), true);
    gestures.hover(other, descriptor);
    assert.equal(gestures.scrub(other, descriptor, 1, 200, 1), false);
    assert.deepEqual(gestures.pointerControl(), control);
    gestures.moveBy(1, 10);
    assert.equal(gestures.state(control)?.value, 450);
    assert.equal(gestures.state(other), undefined);
    gestures.release(1);
    gestures.hover(other, descriptor);
    assert.equal(gestures.scrub(other, descriptor, 1, 200, 1), true);
    assert.deepEqual(gestures.pointerControl(), other);
    assert.deepEqual(actions, [
        { action: "hover", control },
        { action: "begin", interactionId: "drag-1", control, value: 440 },
        { action: "update", interactionId: "drag-1", value: 450 },
        { action: "end", interactionId: "drag-1", value: 450 },
        { action: "hover", control: other },
        { action: "begin", interactionId: "drag-2", control: other, value: 440 },
    ]);
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

test("wheel sensitivity changes preserve the current value and pointer origin", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "drag", () => {});
    gestures.scrub(control, { ...descriptor, value: 0 }, 1, 0, 1);
    gestures.move(1, 10);
    assert.equal(gestures.state(control)?.value, 10);
    const beforeWheel = actions.length;
    assert.equal(gestures.changeSensitivity(100), true);
    assert.equal(gestures.state(control)?.value, 10);
    assert.equal(actions.length, beforeWheel);
    gestures.move(1, 12);
    assert.equal(gestures.state(control)?.value, 11);
    gestures.changeSensitivity(-100);
    gestures.move(1, 13);
    assert.equal(gestures.state(control)?.value, 12);
    gestures.release(1);
    assert.equal(gestures.changeSensitivity(100), false);
});

test("each nonzero wheel event doubles or halves sensitivity regardless of its pixel delta", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(control, { ...descriptor, value: 0 }, 1, 0, 1);
    for (const delta of [3, 120, 1]) gestures.changeSensitivity(delta);
    assert.match(gestures.hint()!.text, /0.125×/);
    gestures.moveBy(1, 8);
    assert.equal(gestures.state(control)?.value, 1);
    for (const delta of [-1, -3, -120]) gestures.changeSensitivity(delta);
    assert.match(gestures.hint()!.text, /1×/);
});

test("capture-induced pointer leave does not end hover before release", () => {
    const actions: Array<{ action: string }> = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "drag", () => {});
    gestures.hover(control, descriptor);
    gestures.scrub(control, descriptor, 1, 0, 1);
    gestures.unhover(control);
    assert.equal(actions.filter((action) => action.action === "hover").length, 1);
    gestures.release(1);
    gestures.unhover(control);
    assert.equal(actions.filter((action) => action.action === "hover").length, 2);
});

test("default snapping adds travel without losing nearby values, even when sensitivity changes inside it", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(control, { ...descriptor, value: -2 }, 1, 0, 1, { defaultValue: 0 });
    gestures.move(1, 2); // Reach the default from below.
    assert.equal(gestures.state(control)?.value, 0);
    gestures.move(1, 8); // Halfway through the extra twelve pixels.
    assert.equal(gestures.state(control)?.value, 0);
    gestures.changeSensitivity(100); // Halve the sensitivity without resetting progress.
    gestures.move(1, 14);
    assert.equal(gestures.state(control)?.value, 0);
    gestures.move(1, 15);
    assert.equal(gestures.state(control)?.value, .5);
    gestures.move(1, 14.01);
    assert.ok(Math.abs(gestures.state(control)!.value - .005) < 1e-12);
    gestures.move(1, 14);
    assert.equal(gestures.state(control)?.value, 0);
    gestures.move(1, 2); // Returning across the same plateau consumes the same distance.
    assert.equal(gestures.state(control)?.value, 0);
    gestures.move(1, 1.99);
    assert.ok(Math.abs(gestures.state(control)!.value + .005) < 1e-12);
});

test("clamping does not accumulate hidden travel at an input limit", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(control, { ...descriptor, value: 5 }, 1, 0, 1, { min: 0, max: 10 });
    gestures.move(1, 100);
    assert.equal(gestures.state(control)?.value, 10);
    gestures.move(1, 99);
    assert.equal(gestures.state(control)?.value, 9);
});

test("numeric entry commits one exact gesture and cancellation sends no edit", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "entry", () => {});
    const limits = { min: 0, max: 1, defaultValue: 0 };
    const anchor = { left: 10, top: 10, bottom: 40 };
    gestures.openEntry(control, { ...descriptor, value: .5 }, limits, anchor);
    assert.equal(gestures.isEditingNode(control.node), true);
    gestures.setEntryDraft("0.00001");
    gestures.cancelEntry();
    assert.deepEqual(actions, []);
    assert.equal(gestures.isEditingNode(control.node), false);
    gestures.openEntry(control, { ...descriptor, value: .5 }, limits, anchor);
    gestures.setEntryDraft("0.00001");
    assert.equal(gestures.acceptEntry(), true);
    assert.deepEqual(actions, [
        { action: "begin", interactionId: "entry", control, value: .5 },
        { action: "update", interactionId: "entry", value: .00001 },
        { action: "end", interactionId: "entry", value: .00001 },
    ]);
    assert.equal(gestures.numericEntry(), null);
});

test("invalid numeric entries stay open without changing the project", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "entry", () => {});
    gestures.openEntry(control, descriptor, { min: 0, max: 500 }, { left: 0, top: 0, bottom: 30 });
    for (const draft of ["", " ", "no", "Infinity", "501", "-1"]) {
        gestures.setEntryDraft(draft);
        assert.equal(gestures.acceptEntry(), false);
        assert.ok(gestures.numericEntry()?.error);
    }
    assert.deepEqual(actions, []);
});

test("reset uses the declared default and can reanchor an ongoing drag", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "reset", () => {});
    gestures.hover(control, { ...descriptor, value: 12 }, { defaultValue: 7 });
    assert.equal(gestures.resetHovered(), true);
    assert.deepEqual(actions.slice(-2), [
        { action: "update", interactionId: "reset", value: 7 },
        { action: "end", interactionId: "reset", value: 7 },
    ]);
    gestures.scrub(control, { ...descriptor, value: 12 }, 1, 0, 1, { defaultValue: 7 });
    gestures.move(1, 5);
    gestures.resetHovered();
    assert.equal(gestures.state(control)?.value, 7);
    gestures.move(1, 12); // Seven pixels from the reset, including six at the default.
    assert.equal(gestures.state(control)?.value, 8);
    gestures.release(1);
});

test("interaction hints describe the current gesture and clear with its hover", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    assert.equal(gestures.hint(), null);
    gestures.hover(control, descriptor, { defaultValue: 440 });
    assert.match(gestures.hint()!.text, /double-click reset/);
    gestures.scrub(control, descriptor, 1, 0, 1);
    gestures.changeSensitivity(100);
    assert.match(gestures.hint()!.text, /0.5×/);
    gestures.release(1);
    gestures.hover(null);
    assert.equal(gestures.hint(), null);
});

test("release keeps the final displayed value while older acknowledgements arrive", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(control, descriptor, 1, 0, 1);
    gestures.move(1, 10);
    gestures.release(1);
    gestures.receive([{ interactionId: "drag", control, value: 440, phase: "active" }]);
    assert.equal(gestures.state(control)?.value, 450);
    gestures.receive([{ interactionId: "drag", control, value: 450, phase: "settled" }]);
    assert.equal(gestures.state(control)?.value, 450);
    gestures.receive([{ interactionId: "drag", control, value: 440, phase: "settled" }]);
    assert.equal(gestures.state(control)?.value, 440); // A later undo remains visible.
});

test("quick successive gestures from one view close in order even while the first begin is pending", async () => {
    const begin = deferred();
    let first = true;
    const { manager, calls } = harness({ beginManualControl: async (_id, _control, value) => {
        calls.push({ action: "begin", value });
        if (first) { first = false; await begin.promise; }
    } });
    manager.action("panel", { action: "begin", interactionId: "first", control, value: 440 });
    await Promise.resolve();
    manager.action("panel", { action: "end", interactionId: "first", value: 450 });
    assert.doesNotThrow(() => manager.action("panel", {
        action: "begin", interactionId: "second", control, value: 450,
    }));
    manager.action("panel", { action: "end", interactionId: "second", value: 470 });
    begin.resolve();
    await manager.settled();
    assert.deepEqual(calls.map(({ action, value }) => [action, value]), [
        ["begin", 440], ["end", 450], ["begin", 470], ["end", 470],
    ]);
    assert.equal(manager.states("panel").length, 1);
    assert.equal(manager.states("panel")[0].value, 470);
});

test("a different view cannot take over an input while its previous gesture is ending", async () => {
    const end = deferred();
    const { manager } = harness({ endManualControl: async () => end.promise });
    manager.action("a", { action: "begin", interactionId: "first", control, value: 440 });
    await manager.settled();
    manager.action("a", { action: "end", interactionId: "first", value: 450 });
    assert.throws(() => manager.action("b", { action: "begin", interactionId: "other", control, value: 450 }), /active interaction/);
    end.resolve(); await manager.settled();
});

test("a queued predecessor end is not sent twice and its final value survives a rejected re-grab", async () => {
    let begins = 0;
    const { manager, calls } = harness({ beginManualControl: async (_id, _control, value) => {
        calls.push({ action: "begin", value });
        if (++begins === 2) throw new Error("input changed");
    } });
    manager.action("panel", { action: "begin", interactionId: "first", control, value: 440 });
    await manager.settled();
    manager.action("panel", { action: "end", interactionId: "first", value: 450 });
    manager.action("panel", { action: "begin", interactionId: "second", control, value: 470 });
    await manager.settled();
    assert.deepEqual(calls.map(({ action, value }) => [action, value]), [
        ["begin", 440], ["end", 450], ["begin", 470],
    ]);
    assert.equal(manager.states("panel").length, 1);
    assert.equal(manager.states("panel")[0].phase, "failed");
    assert.equal(manager.states("panel")[0].value, 450);
});

test("metadata refresh changes reset and numeric limits without replacing a typed draft", () => {
    const gestures = new ManualControlGestures(() => {}, () => "entry", () => {});
    gestures.hover(control, descriptor, { defaultValue: 440 });
    gestures.openEntry(control, descriptor, { min: 0, max: 1000 }, { left: 0, top: 0, bottom: 30 });
    gestures.setEntryDraft("600");
    gestures.refresh(control, descriptor, { min: 0, max: 500, defaultValue: 300 });
    assert.equal(gestures.numericEntry()?.draft, "600");
    assert.equal(gestures.acceptEntry(), false);
    gestures.cancelEntry();
    gestures.resetHovered();
    assert.equal(gestures.state(control)?.value, 300);
});

test("a webview shows another holder's live value until its own gesture takes over", () => {
    const gestures = new ManualControlGestures(() => {}, () => "own", () => {});
    assert.equal(gestures.liveValue(control), undefined);
    gestures.receive([], [{ control, value: 470 }]);
    assert.equal(gestures.liveValue(control), 470);
    assert.equal(gestures.state(control), undefined);
    gestures.edit(control, descriptor);
    gestures.update(control, 480);
    assert.equal(gestures.state(control)?.value, 480);
    gestures.receive([], []);
    assert.equal(gestures.liveValue(control), undefined);
});

test("disconnect clears relayed values before a replacement session uses the same control identity", () => {
    const gestures = new ManualControlGestures(() => {}, () => "own", () => {});
    gestures.receive([], [{ control, value: 470 }]);
    gestures.disconnected();
    assert.equal(gestures.liveValue(control), undefined);
    assert.equal(gestures.state(control), undefined);
});

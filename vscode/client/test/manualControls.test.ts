import test from "node:test";
import assert from "node:assert/strict";
import { ManualControlInteractions, manualControlKey, resolveManualControl, type ManualControlDescriptor, type ManualControlId,
    type ManualControlTransport } from "../src/manualControls";
import { ManualControlGestures } from "../src/manualControlGesture";
import { formatStatusHint, playbackShortcut } from "../src/statusBarText";
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

test("a disabled input knob can prepare, scrub, and release without enabling it", async () => {
    const { manager, calls } = harness();
    const graph = scope();
    const disabled = graph.nodes[0].sampleInputs[0].manualControl!;
    disabled.enabled = false;
    manager.reconcile(graph);
    const gestures = new ManualControlGestures((action) => manager.action("panel", action), () => "drag", () => {});
    gestures.hover(control);
    await manager.settled();
    gestures.scrub(control, disabled, 7, 100, 2);
    gestures.move(7, 120, 20, 20000);
    gestures.release(7);
    await manager.settled();
    gestures.hover(null);
    await manager.settled();
    assert.deepEqual(calls.map(({ action, value }) => [action, value]),
        [["hover", undefined], ["begin", 480], ["end", 480], ["hover", undefined]]);
    assert.equal(resolveManualControl(graph, control)?.enabled, false);
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

test("Ctrl precision starts at one quarter speed and the wheel still changes it by powers of two", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(control, { ...descriptor, value: 0 }, 1, 0, 2, {}, true);
    assert.match(gestures.hint()!.text, /0.25×/);
    gestures.moveBy(1, 8);
    assert.equal(gestures.state(control)?.value, 4);
    gestures.changeSensitivity(-1);
    assert.match(gestures.hint()!.text, /0.5×/);
    gestures.moveBy(1, 8);
    assert.equal(gestures.state(control)?.value, 12);
    gestures.release(1);
    gestures.scrub(control, descriptor, 2, 0, 2);
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
    gestures.move(1, 14); // Halfway through the extra twenty-four pixels.
    assert.equal(gestures.state(control)?.value, 0);
    gestures.changeSensitivity(100); // Halve the sensitivity without resetting progress.
    gestures.move(1, 26);
    assert.equal(gestures.state(control)?.value, 0);
    gestures.move(1, 27);
    assert.equal(gestures.state(control)?.value, .5);
    gestures.move(1, 26.01);
    assert.ok(Math.abs(gestures.state(control)!.value - .005) < 1e-12);
    gestures.move(1, 26);
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

test("output gain scrubbing reaches exact zero independently of pointer event count", () => {
    const gain: ManualControlId = { node: control.node, outputName: "main" };
    const limits = { defaultValue: 1, min: 0 };
    const make = () => {
        const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
        assert.equal(gestures.scrub(gain, { ...descriptor, value: 1 }, 1, 0, .01, limits), true);
        return gestures;
    };
    const single = make();
    single.moveBy(1, -112); // Twelve pixels leave the default plateau, then 100 scale the value to zero.
    const repeated = make();
    for (let i = 0; i < 112; ++i) repeated.moveBy(1, -1);
    assert.equal(single.state(gain)?.value, 0);
    assert.equal(repeated.state(gain)?.value, 0);
    repeated.moveBy(1, .01);
    assert.ok(Math.abs(repeated.state(gain)!.value - .0001) < 1e-12);
    assert.notEqual(repeated.state(gain)?.value, 0); // Nearby values remain reachable.
});

test("inverted output scrubbing changes magnitude, clamps at zero and retains polarity", () => {
    const gain: ManualControlId = { node: control.node, outputName: "main" };
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    gestures.scrub(gain, { ...descriptor, value: -.5 }, 1, 0, .01, { defaultValue: 1, min: 0 });
    gestures.moveBy(1, 10);
    assert.ok(Math.abs(gestures.state(gain)!.value + .6) < 1e-12);
    assert.ok(Math.abs(gestures.displayValue(gain, gestures.state(gain)!.value) - .6) < 1e-12);
    gestures.moveBy(1, -1000);
    assert.equal(gestures.state(gain)?.value, 0);
    assert.equal(gestures.outputInverted(gain, 0), true);
    gestures.moveBy(1, 10);
    assert.ok(Math.abs(gestures.state(gain)!.value + .1) < 1e-12);
});

test("output inversion flips the signed gain in one gesture without changing magnitude", () => {
    const gain: ManualControlId = { node: control.node, outputName: "main" };
    const actions: Array<{ action: string; value?: number }> = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "invert", () => {});
    assert.equal(gestures.invertOutput(gain, { ...descriptor, value: .75 }), true);
    assert.deepEqual(actions.map(({ action, value }) => [action, value]), [
        ["begin", .75], ["update", -.75], ["end", -.75],
    ]);
    assert.equal(gestures.displayValue(gain, gestures.state(gain)!.value), .75);
    assert.equal(gestures.outputInverted(gain, gestures.state(gain)!.value), true);
    gestures.receive([{ interactionId: "invert", control: gain, value: .75, phase: "settled" }]);
    assert.equal(gestures.outputInverted(gain, gestures.state(gain)!.value), false);
});

test("inversion at zero is local and applies when the magnitude increases", () => {
    const gain: ManualControlId = { node: control.node, outputName: "main" };
    const actions: Array<{ action: string }> = [];
    let saved: Record<string, boolean> = {};
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "drag", () => {},
        { save: (inverted) => { saved = inverted; } });
    const zero = { ...descriptor, value: 0 };
    assert.equal(gestures.invertOutput(gain, zero), true);
    assert.equal(actions.length, 0); // The server's zero is unchanged.
    assert.equal(gestures.outputInverted(gain, 0), true);
    assert.equal(saved[manualControlKey(gain)], true);
    const reopened = new ManualControlGestures(() => {}, () => "drag", () => {}, { initial: saved });
    reopened.scrub(gain, zero, 1, 0, .01, { defaultValue: 1, min: 0 });
    reopened.moveBy(1, 10);
    assert.ok(Math.abs(reopened.state(gain)!.value + .1) < 1e-12);
    assert.equal(gestures.valueFromDisplay(control, -2), -2); // Input controls retain their signed range.
});

test("inverted gain numeric entry and default reset edit magnitude and preserve polarity", () => {
    const gain: ManualControlId = { node: control.node, outputName: "main" };
    let next = 0;
    const gestures = new ManualControlGestures(() => {}, () => `gain:${++next}`, () => {});
    const inverted = { ...descriptor, value: -.5 };
    const limits = { min: 0, defaultValue: 1 };
    gestures.openEntry(gain, inverted, limits, { left: 0, top: 0, bottom: 30 });
    assert.equal(gestures.numericEntry()?.draft, "0.5");
    gestures.setEntryDraft("-0.25");
    assert.equal(gestures.acceptEntry(), false);
    gestures.setEntryDraft("0.25");
    assert.equal(gestures.acceptEntry(), true);
    assert.equal(gestures.state(gain)?.value, -.25);
    gestures.hover(gain, inverted, limits);
    assert.equal(gestures.resetHovered(), true);
    assert.equal(gestures.state(gain)?.value, -1);
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
    gestures.move(1, 18); // Thirteen pixels from the reset, including twelve at the default.
    assert.equal(gestures.state(control)?.value, 8);
    gestures.release(1);
});

test("double-click reset edits the value of a disabled input", () => {
    const actions: unknown[] = [];
    const gestures = new ManualControlGestures((action) => actions.push(action), () => "reset", () => {});
    gestures.hover(control, { ...descriptor, enabled: false, participates: false, value: 2 }, { defaultValue: 1 });
    assert.equal(gestures.resetHovered(), true);
    assert.deepEqual(actions.slice(-2), [
        { action: "update", interactionId: "reset", value: 1 },
        { action: "end", interactionId: "reset", value: 1 },
    ]);
});

test("interaction hints describe the current gesture and clear with its hover", () => {
    const gestures = new ManualControlGestures(() => {}, () => "drag", () => {});
    assert.equal(gestures.hint(), null);
    gestures.hover(control, descriptor, { defaultValue: 440 }, "Sequential input");
    assert.match(formatStatusHint(gestures.hint()!, false), /^Sequential input.*↑↓.*🖱︎R value.*Dbl-click reset/);
    assert.equal(formatStatusHint(gestures.hint()!, true), "Sequential input · frequency 440");
    gestures.refresh(control, { ...descriptor, value: 450 }, { defaultValue: 440 });
    assert.match(gestures.hint()!.text, /^Sequential input · frequency 450/);
    gestures.scrub(control, descriptor, 1, 0, 1);
    gestures.changeSensitivity(100);
    assert.match(gestures.hint()!.text, /0.5×/);
    assert.match(formatStatusHint(gestures.hint()!, false), /0.5× · Wheel .* · Esc release/);
    assert.equal(formatStatusHint(gestures.hint()!, true), "frequency 440 · 0.5×");
    gestures.release(1);
    gestures.hover(null);
    assert.equal(gestures.hint(), null);
});

test("a shortcut-only status has no text when shortcuts are hidden", () => {
    const hint = { shortcuts: playbackShortcut };
    assert.equal(formatStatusHint(hint, false), playbackShortcut);
    assert.equal(formatStatusHint(hint, true), "");
    const error = { text: "Pointer lock unavailable", shortcuts: "Esc release" };
    assert.equal(formatStatusHint(error, true), error.text);
});

test("a disabled knob distinguishes predictive hover, active drag and held override", () => {
    let nextId = 0;
    const gestures = new ManualControlGestures(() => {}, () => `drag:${++nextId}`, () => {});
    const disabled = { ...descriptor, enabled: false, participates: false };
    assert.equal(gestures.activity(control), "idle");
    gestures.hover(control, disabled);
    assert.equal(gestures.activity(control), "hover");
    gestures.scrub(control, disabled, 1, 0, 1);
    assert.equal(gestures.activity(control), "active");
    gestures.release(1);
    assert.equal(gestures.activity(control), "retained");
    gestures.receive([{ interactionId: "drag:1", control, value: 440, phase: "settled" }]);
    assert.equal(gestures.activity(control), "retained");
    gestures.unhover(control);
    assert.equal(gestures.activity(control), "idle");
    gestures.hover(control, disabled);
    assert.equal(gestures.activity(control), "hover");
    gestures.scrub(control, disabled, 1, 0, 1);
    gestures.release(1);
    gestures.receive([{ interactionId: "drag:2", control, value: 440, phase: "failed" }]);
    assert.equal(gestures.activity(control), "hover");
    assert.equal(disabled.enabled, false);
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

test("a new graph generation retires a knob error and allows another edit", async () => {
    let begins = 0;
    const { manager } = harness({ beginManualControl: async () => {
        if (++begins === 1) throw new Error("Control compilation failed");
    } });
    manager.action("panel", { action: "begin", interactionId: "first", control, value: 440 });
    await manager.settled();
    assert.equal(manager.states("panel")[0].graphGeneration, 1);
    manager.reconcile(scope());
    assert.equal(manager.states("panel")[0].phase, "failed");
    manager.reconcile({ ...scope(), generation: 2 });
    assert.deepEqual(manager.states("panel"), []);
    manager.action("panel", { action: "begin", interactionId: "retry", control, value: 440 });
    await manager.settled();
    assert.equal(manager.states("panel")[0].phase, "active");
});

test("a late failure closes the optimistic gesture without marking the replacement graph", () => {
    const gestures = new ManualControlGestures(() => {}, () => "old", () => {});
    gestures.configuredGeneration(1);
    gestures.edit(control, descriptor);
    gestures.finish(control);
    gestures.configuredGeneration(2);
    gestures.receive([{ interactionId: "old", control, value: 440, phase: "failed", graphGeneration: 1, error: "old graph" }]);
    assert.equal(gestures.state(control), undefined);
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


test("output gains keep their identity and gesture lifetime separate from same-named inputs", async () => {
    const gain: ManualControlId = { node: control.node, outputName: "frequency" };
    assert.notEqual(manualControlKey(gain), manualControlKey(control));
    const graph = scope();
    graph.nodes[0].sampleOutputs = [{ name: "frequency", index: 0,
        manualControl: { ...descriptor, value: 1 } }];
    assert.equal(resolveManualControl(graph, control)?.value, 440);
    assert.equal(resolveManualControl(graph, gain)?.value, 1);
    const controls: ManualControlId[] = [];
    const { manager, calls } = harness({ beginManualControl: async (_id, id, value) => {
        controls.push(id); calls.push({ action: "begin", value });
    } });
    manager.reconcile(graph);
    manager.action("lane", { action: "begin", interactionId: "gain", control: gain, value: 1 });
    await manager.settled();
    manager.action("lane", { action: "update", interactionId: "gain", value: -.5 });
    manager.action("lane", { action: "end", interactionId: "gain", value: -.25 });
    await manager.settled();
    assert.deepEqual(controls, [gain]);
    assert.deepEqual(calls.map(({ action, value }) => [action, value]), [
        ["begin", 1], ["update", -.5], ["end", -.25],
    ]);
    assert.equal(manager.states("lane")[0].value, -.25);
});

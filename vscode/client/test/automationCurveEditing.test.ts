import test from "node:test";
import assert from "node:assert/strict";
import { AutomationCurveEditing, automationCycleSeconds, automationWindow, readAutomationCurve, type CurveAxes } from "../src/automationCurveEditing";
import type { GraphScope, NodeAddress } from "../src/graphScope";
import { serializeNodeEvent, type NodeEvent } from "../src/nodeEvents";

const target: NodeAddress = { instanceId: "curve" };
const axes: CurveAxes = { left: 0, top: 0, width: 100, height: 100, begin: 0, end: 10, min: 0, max: 1 };
const construction = { definitionId: "iv.builtin.automation.curve", arguments: [[
    { id: 1, seconds: 1, value: .5, interpolation: "bezier", tension: 0 },
    { id: 2, seconds: 5, value: 1, interpolation: "bezier", tension: 0 },
]] };
function scope(revision = 1, value = construction): GraphScope {
    return { sessionRevision: revision, generation: revision, editable: true, connections: [], nodes: [{ address: target,
        loadState: "ready", instance: { instanceId: "curve", definitionId: "iv.builtin.automation.curve", realized: true },
        sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [], hasChildren: true, construction: value }] };
}
function harness(send: (event: NodeEvent) => Promise<void> = async () => {}, revision?: number) {
    let callback: (() => void) | null = null;
    let closed = 0;
    const direct: unknown[] = [];
    const edits = new AutomationCurveEditing({ send: async (_target, event) => { direct.push(serializeNodeEvent(event)); },
        gesture: () => ({ send, finish: async () => { ++closed; }, acceptedRevision: () => revision }) }, () => {},
        { set: (run) => { callback = run; return 1; }, clear: () => { callback = null; } });
    edits.reconcile(scope());
    return { edits, direct, runTimer: () => { const run = callback; callback = null; run?.(); }, closed: () => closed };
}

test("point drags retain the grab offset and a click without movement makes no semantic edit", async () => {
    let sent = 0;
    const h = harness(async () => { ++sent; });
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 12, y: 53 }), true);
    h.edits.move(7, { x: 12, y: 53 });
    await h.edits.finish();
    assert.equal(sent, 0);
    assert.equal(h.edits.pointerId(), null);
});

test("coalescing keeps the latest point while a message runs, and release flushes it before history closes", async () => {
    let release!: () => void;
    const first = new Promise<void>((resolve) => { release = resolve; });
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); if (sent.length === 1) await first; });
    h.edits.begin(target, 1, 7, axes);
    h.edits.move(7, { x: 20, y: 50 });
    h.edits.move(7, { x: 30, y: 50 });
    h.runTimer();
    assert.deepEqual(sent, [{ id: 1, seconds: 3, value: .5 }]);
    // A component remount only changes who reads this store.
    const remountedView = () => h.edits.drafts(target);
    h.edits.move(7, { x: 40, y: 25 });
    assert.deepEqual(remountedView(), [{ id: 1, seconds: 4, value: .75 }]);
    const finished = h.edits.finish();
    assert.equal(h.closed(), 0);
    release(); await finished;
    assert.deepEqual(sent, [{ id: 1, seconds: 3, value: .5 }, { id: 1, seconds: 4, value: .75 }]);
    assert.equal(h.closed(), 1);
});

test("the released draft survives old reads and yields to the accepted revision including another client's newer edit", async () => {
    const h = harness(undefined, 8);
    h.edits.begin(target, 1, 7, axes);
    h.edits.move(7, { x: 40, y: 25 });
    await h.edits.finish();
    h.edits.reconcile(scope(7));
    assert.deepEqual(h.edits.drafts(target), [{ id: 1, seconds: 4, value: .75 }]);
    const other = structuredClone(construction);
    (other.arguments[0] as Array<{ value: number }>)[0].value = .25;
    h.edits.reconcile(scope(9, other));
    assert.deepEqual(h.edits.drafts(target), []);
    assert.equal(h.edits.view(target)!.points[0].value, .25);
    h.edits.reconcile(scope(7));
    assert.equal(h.edits.view(target)!.points[0].value, .25);
});

test("a failed desired configuration does not erase the accepted curve", () => {
    const h = harness();
    const failed = scope(2);
    failed.nodes[0].loadState = "failed";
    failed.nodes[0].instance.configurationSource = "invalid new source";
    h.edits.reconcile(failed);
    assert.deepEqual(h.edits.view(target)?.points, construction.arguments[0]);
});

test("removed points discard unsent movement, and conflicts restore the accepted data", async () => {
    let sent = 0;
    const h = harness(async () => { ++sent; throw new Error("gesture conflicted"); });
    h.edits.begin(target, 1, 7, axes);
    h.edits.move(7, { x: 40, y: 25 });
    h.edits.reconcile({ ...scope(2), nodes: [] });
    await h.edits.finish();
    assert.equal(sent, 0);
    h.edits.reconcile(scope(3));
    h.edits.begin(target, 1, 8, axes);
    h.edits.move(8, { x: 40, y: 25 });
    await h.edits.finish();
    assert.equal(sent, 1);
    assert.deepEqual(h.edits.drafts(target), []);
    assert.match(h.edits.error(target), /conflicted/);
    assert.equal(h.edits.view(target)!.points[0].value, .5);
});

test("construction reads validate IDs and render narrow windows with neighboring segments and equal-position steps", () => {
    const data = readAutomationCurve(construction)!;
    assert.deepEqual(automationWindow(data, 2, 3).points, []);
    assert.deepEqual(automationWindow(data, 2, 3).vertices, [{ seconds: 2, value: .625 }, { seconds: 3, value: .75 }]);
    // Before the first point the curve is on the wrap segment from the last
    // point; without a compiled sample rate that segment is level.
    assert.deepEqual(automationWindow(data, 0, 2).vertices, [
        { seconds: 0, value: .5 }, { seconds: 1, value: .5 }, { seconds: 2, value: .625 },
    ]);
    const duplicate = structuredClone(construction);
    (duplicate.arguments[0] as unknown[]).push({ id: 1, seconds: 2, value: 0 });
    assert.equal(readAutomationCurve(duplicate), null);
    const many = { cycleSeconds: Infinity, points: Array.from({ length: 10000 }, (_, id) => ({ id, seconds: id, value: id % 2, interpolation: "bezier" as const, tension: 0 })) };
    assert.equal(automationWindow(many, 5000, 5001).points.length, 2);
    const step = { cycleSeconds: Infinity, points: [{ id: 1, seconds: 1, value: 0, interpolation: "bezier" as const, tension: 0 },
        { id: 2, seconds: 1, value: 1, interpolation: "bezier" as const, tension: 0 }] };
    assert.equal(automationWindow(step, 2, 3).vertices[0].value, 1);
});


test("successive point drags preserve each pending preview until its accepted scope arrives", async () => {
    const h = harness(undefined, 8);
    h.edits.begin(target, 1, 7, axes);
    h.edits.move(7, { x: 20, y: 50 });
    await h.edits.finish();
    h.edits.begin(target, 2, 8, axes);
    h.edits.move(8, { x: 70, y: 25 });
    assert.deepEqual(h.edits.drafts(target), [
        { id: 1, seconds: 2, value: .5 }, { id: 2, seconds: 7, value: .75 },
    ]);
    await h.edits.finish();
    const window = automationWindow(h.edits.view(target)!, 0, 10, h.edits.drafts(target));
    assert.deepEqual(window.points.map((point) => point.seconds), [2, 7]);
    h.edits.reconcile(scope(8));
    assert.deepEqual(h.edits.drafts(target), []);
});

test("the curve is cyclic: the last point interpolates toward the first across the global wrap", () => {
    const cycle = automationCycleSeconds(48000);
    assert.equal(cycle, 2 ** 64 / 48000);
    assert.equal(automationCycleSeconds(null), Infinity);
    const data = readAutomationCurve(construction, 48000)!;
    assert.equal(data.cycleSeconds, cycle);
    // Just past the last point the value starts moving toward the first one.
    const after = automationWindow(data, 6, 7).vertices[0].value;
    assert.ok(after < 1 && after > 1 - 1e-12);
    // Halfway around the wrap segment it is halfway between the two values.
    const middle = 5 + (cycle - 4) / 2;
    assert.ok(Math.abs(automationWindow(data, middle, middle + 1).vertices[0].value - .75) < 1e-6);
    // Points may lie anywhere, including before zero; there is no duration.
    const signed = readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [[
        { id: 4, seconds: 2, value: 1, interpolation: "bezier", tension: 0 },
        { id: 3, seconds: -2, value: 0, interpolation: "bezier", tension: 0 },
    ]] })!;
    assert.deepEqual(signed.points.map((point) => point.id), [3, 4]);
    assert.deepEqual(automationWindow(signed, -1, 0).vertices, [{ seconds: -1, value: .25 }, { seconds: 0, value: .5 }]);
    assert.equal(readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [[], 10] }), null);
});

test("one point defines a constant curve and no points define empty coverage", () => {
    const single = readAutomationCurve({ definitionId: "iv.builtin.automation.curve",
        arguments: [[{ id: 1, seconds: 3, value: .25, interpolation: "bezier", tension: 1 }]] }, 48000)!;
    assert.deepEqual(automationWindow(single, -10, 10).vertices.map((vertex) => vertex.value), [.25, .25, .25]);
    const outside = automationWindow(single, 20, 30);
    assert.deepEqual(outside.vertices, [{ seconds: 20, value: .25 }, { seconds: 30, value: .25 }]);
    assert.equal(outside.points.length, 0);
    assert.deepEqual(outside.paths.map(({ begin, end }) => [begin, end]),
        [[{ seconds: 20, value: .25 }, { seconds: 30, value: .25 }]]);
    const empty = readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [[]] })!;
    assert.deepEqual(automationWindow(empty, 0, 1).vertices, []);
    assert.deepEqual(automationWindow(empty, 0, 1).paths, []);
});

test("sparse segments render gaps, half-open holds and equal-position jumps", () => {
    const curve = readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [[
        { id: 1, seconds: 0, value: 0, interpolation: "bezier", tension: 0 },
        { id: 3, seconds: 4, value: 3, interpolation: "hold", tension: 1 },
        { id: 2, seconds: 4, value: 1, interpolation: "empty", tension: 1 },
        { id: 4, seconds: 6, value: 5, interpolation: "empty", tension: 1 },
    ]] }, 48000)!;
    assert.deepEqual(curve.points.map((point) => point.id), [1, 2, 3, 4]);
    const paths = automationWindow(curve, -1, 8).paths;
    assert.equal(paths.length, 2);
    assert.deepEqual(paths[0].begin, { seconds: 0, value: 0 });
    assert.deepEqual(paths[0].end, { seconds: 4, value: 1 });
    assert.deepEqual(paths[1].begin, { seconds: 4, value: 3 });
    assert.deepEqual(paths[1].end, { seconds: 6, value: 3 });
    assert.deepEqual(automationWindow(curve, 6, 8).paths, []);
});

test("automatic cubic matches the backend on irregular intervals and tension zero stays exactly linear", () => {
    const curve = readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [[
        { id: 1, seconds: 0, value: 0, interpolation: "bezier", tension: 1 },
        { id: 2, seconds: 4, value: 1, interpolation: "bezier", tension: 1 },
        { id: 3, seconds: 12, value: 2, interpolation: "empty", tension: 1 },
    ]] }, 48000)!;
    const automatic = automationWindow(curve, 2, 3);
    assert.ok(Math.abs(automatic.vertices[0].value - .41346153846153844) < 1e-12);
    assert.ok(automatic.paths[0].first >= 0 && automatic.paths[0].second <= 1);
    const linear = { ...curve, points: curve.points.map((point) => ({ ...point, tension: 0 })) };
    assert.equal(automationWindow(linear, 2, 3).vertices[0].value, .5);
});

test("one empty point remains empty and a hold segment can cross zero", () => {
    const data = (points: unknown[]) => readAutomationCurve({ definitionId: "iv.builtin.automation.curve", arguments: [points] }, 48000)!;
    assert.deepEqual(automationWindow(data([{ id: 1, seconds: 2, value: .25, interpolation: "empty", tension: 1 }]), 0, 4).paths, []);
    const curve = data([{ id: 1, seconds: -2, value: .25, interpolation: "hold", tension: 1 },
        { id: 2, seconds: 2, value: 1, interpolation: "empty", tension: 1 }]);
    const paths = automationWindow(curve, -3, 3).paths;
    assert.equal(paths.length, 1);
    assert.deepEqual(paths[0].begin, { seconds: -2, value: .25 });
    assert.deepEqual(paths[0].end, { seconds: 2, value: .25 });
});

test("segment edits use typed messages and preserve the accepted curve until its replacement arrives", async () => {
    const h = harness();
    await h.edits.segment(target, 1, "empty", .5);
    assert.equal(h.edits.view(target)!.points[0].interpolation, "bezier");
    assert.equal(h.direct.length, 1);
    assert.deepEqual((h.direct[0] as { payload: unknown }).payload, { id: 1, interpolation: "empty", tension: .5 });
    await h.edits.add(target, { seconds: 3, value: .25 });
    const added = (h.direct[1] as { payload: { point: { interpolation: string; tension: number } } }).payload.point;
    assert.equal(added.interpolation, "bezier");
    assert.equal(added.tension, 1);
});

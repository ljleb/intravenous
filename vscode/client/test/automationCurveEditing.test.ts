import test from "node:test";
import assert from "node:assert/strict";
import { AutomationCurveEditing, automationCycleSeconds, automationWindow, curvePointAt, readAutomationCurve, type CurveAxes } from "../src/automationCurveEditing";
import { automationSampleOffset, automationValueAtFraction, automationValueFraction, automationValueGuides,
    panAutomationValueAxis, zoomAutomationValueAxis } from "../src/automationCurveGeometry";
import { readLayout, type GraphScope, type GraphScopeNode, type NodeAddress, type Point } from "../src/graphScope";
import { KnobPointerLock } from "../src/knobPointerLock";
import { knobFraction, valueScaleFraction } from "../src/knobPresentation";
import { serializeNodeEvent, type NodeEvent } from "../src/nodeEvents";
import { wrapTimelineCursor } from "../src/timelineViewport";
import { automationTimelineTicks, timelineTicks } from "../webview/timeline/geometry";

const target: NodeAddress = { instanceId: "curve" };
const axes: CurveAxes = { left: 0, top: 0, width: 100, height: 100, begin: 0, end: 10, min: 0, max: 1 };
const construction = { definitionId: "iv.builtin.automation.curve", arguments: [[
    { id: 1, seconds: 1, value: .5, interpolation: "bezier", tension: 0 },
    { id: 2, seconds: 5, value: 1, interpolation: "bezier", tension: 0 },
]] };
function scope(revision = 1, value: GraphScopeNode["construction"] = construction): GraphScope {
    return { sessionRevision: revision, generation: revision, sampleRate: 48000, editable: true, connections: [], nodes: [{ address: target,
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
    assert.equal(h.edits.selectedPoints(target).size, 0);
    await h.edits.finish();
    assert.equal(sent, 0);
    assert.equal(h.edits.pointerId(), null);
});

test("rectangle selection toggles and unions points, then moves a selected group with one gesture", async () => {
    const moves: unknown[] = [];
    const h = harness(async (event) => { moves.push(serializeNodeEvent(event).payload); });
    h.edits.selectPoints(target, [1]);
    h.edits.selectPoints(target, [1, 2], { ctrlKey: true });
    assert.deepEqual([...h.edits.selectedPoints(target)], [2]);
    h.edits.selectPoints(target, [1], { shiftKey: true });
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }), true);
    h.edits.move(7, { x: 20, y: 20 });
    const points = h.edits.drafts(target);
    assert.deepEqual(points.map(({ id, sampleIndex }) => [id, sampleIndex]), [[1, 96000n], [2, 288000n]]);
    assert.ok(points[0].value > .5);
    assert.ok(Math.abs(points[1].value - points[0].value - .5) < 1e-6);
    h.runTimer();
    assert.deepEqual(moves, []);
    await h.edits.finish();
    assert.deepEqual(moves, [
        { id: 1, seconds: 0, sample_index: "96000", value: points[0].value },
        { id: 2, seconds: 0, sample_index: "288000", value: points[1].value },
    ]);
    assert.equal(h.closed(), 1);
});

test("dragging an unselected control point preserves a separate selection", async () => {
    const moves: number[] = [];
    const h = harness(async (event) => { moves.push((serializeNodeEvent(event).payload as { id: number }).id); });
    h.edits.selectPoints(target, [2]);
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }), true);
    h.edits.move(7, { x: 20, y: 50 });
    assert.deepEqual([...h.edits.selectedPoints(target)], [2]);
    assert.deepEqual(h.edits.drafts(target).map((point) => point.id), [1]);
    await h.edits.finish();
    assert.deepEqual(moves, [1]);
    assert.deepEqual([...h.edits.selectedPoints(target)], [2]);
});

test("cropped values remain linear and the window stops at the enabled bounds", () => {
    const axis = { min: -4, max: 8, pan: 2, display: "cropped" } as const;
    assert.equal(automationValueFraction(1, axis), 0);
    assert.equal(automationValueFraction(3, axis), 1);
    assert.equal(automationValueFraction(4, axis), 1.5);
    assert.equal(automationValueAtFraction(.25, axis), 1.5);
    assert.equal(panAutomationValueAxis(axis, 400, 100), 7);
    assert.equal(panAutomationValueAxis(axis, -400, 100), -3);
    assert.equal(automationValueAtFraction(1, { ...axis, min: 7.5 }), 8);
});

test("lane value zoom holds the value at the cursor in both display modes", () => {
    const cursor = .75;
    for (const display of ["curved", "cropped"] as const) {
        const axis = { display, pan: .25 };
        const value = automationValueAtFraction(cursor, axis);
        const zoomed = { ...axis, ...zoomAutomationValueAxis(axis, 4, cursor) };
        assert.ok(Math.abs(automationValueFraction(value, zoomed) - cursor) < 1e-9);
        assert.ok(automationValueAtFraction(1, zoomed) < automationValueAtFraction(1, axis));
    }
    const bounded = { display: "cropped", min: 0, max: .25 } as const;
    const zoomed = { ...bounded, ...zoomAutomationValueAxis(bounded, 4, 0) };
    assert.equal(automationValueAtFraction(0, zoomed), 0);
    assert.equal(automationValueAtFraction(1, zoomed), .0625);
    const zoomedOut = { ...bounded, ...zoomAutomationValueAxis(bounded, .25, 0) };
    assert.equal(zoomedOut.zoom.factor, 1);
    assert.equal(automationValueAtFraction(0, zoomedOut), 0);
    assert.equal(automationValueAtFraction(1, zoomedOut), .25);
});

test("curved value zoom can display a wider range without losing its finite bounds", () => {
    const anchor = .625;
    const axis = { pan: 1 };
    const value = automationValueAtFraction(anchor, axis);
    const zoomedOut = zoomAutomationValueAxis(axis, 1 / 256, anchor);
    assert.equal(zoomedOut.zoom.factor, 1 / 256);
    assert.ok(Math.abs(automationValueFraction(value, zoomedOut) - anchor) < 1e-10);
    assert.ok(Math.abs(automationValueAtFraction(.25, zoomedOut) - automationValueAtFraction(.75, zoomedOut)) > 500);
    assert.deepEqual(readLayout({ automationValueZooms: { curve: zoomedOut.zoom } }).automationValueZooms,
        { curve: zoomedOut.zoom });

    const lowerBound = { min: 2 };
    const upperBound = { max: -2 };
    assert.equal(automationValueAtFraction(0, { ...lowerBound, ...zoomAutomationValueAxis(lowerBound, .01, .5) }), 2);
    assert.equal(automationValueAtFraction(1, { ...upperBound, ...zoomAutomationValueAxis(upperBound, .01, .5) }), -2);
    assert.equal(zoomAutomationValueAxis({ min: 2, max: 4 }, .01, .5).zoom.factor, 1);
});

test("lane value zoom uses a vertical anchor beyond the wrapped lane edge", () => {
    const curved = { zoom: { factor: 8, center: .5 } };
    const cropped = { ...curved, display: "cropped" as const };
    for (const axis of [curved, cropped]) {
        const beyond = zoomAutomationValueAxis(axis, 2, 1.5);
        const value = automationValueAtFraction(1.5, axis);
        assert.ok(Math.abs(automationValueFraction(value, { ...axis, ...beyond }) - 1.5) < 1e-10);
        assert.notDeepEqual(beyond, zoomAutomationValueAxis(axis, 2, 1));
    }
});

test("curved lane zoom stays continuous through successive vertical cursor wraps", () => {
    const lane = { left: 0, top: 0, width: 200, height: 100 };
    const cursor = { x: 0, y: 50 };
    const axis = { zoom: { factor: .5, center: .5 } };
    const zoomAtTravel = (y: number) => {
        const position = wrapTimelineCursor(cursor, { x: 0, y }, lane);
        return { position, view: zoomAutomationValueAxis(axis, 1.01, 1 - position.virtual.y / lane.height) };
    };
    for (const edge of [-150, -50, 50, 150]) {
        const before = zoomAtTravel(edge - .01);
        const after = zoomAtTravel(edge + .01);
        assert.equal(after.position.screens.y, before.position.screens.y + 1);
        assert.ok(before.position.visible.y - after.position.visible.y > 99);
        assert.ok(before.view.pan > after.view.pan);
        assert.ok(before.view.pan - after.view.pan < .001);
    }
    assert.ok(zoomAtTravel(-200).view.pan > zoomAtTravel(-100).view.pan);
    assert.ok(zoomAtTravel(200).view.pan < zoomAtTravel(100).view.pan);
});

test("crossing the default curved zoom gives the same view in one or two steps", () => {
    const axis = { pan: 1, zoom: { factor: .5, center: .5 } };
    for (const anchor of [-1.5, -.01, .625, .99, 2.5]) {
        const direct = zoomAutomationValueAxis(axis, 4, anchor);
        const atDefault = zoomAutomationValueAxis(axis, 2, anchor);
        assert.deepEqual(direct, zoomAutomationValueAxis(atDefault, 2, anchor));
        assert.deepEqual(atDefault, zoomAutomationValueAxis(direct, .5, anchor));
        const restored = zoomAutomationValueAxis(direct, .25, anchor);
        assert.equal(restored.zoom.factor, axis.zoom.factor);
        assert.equal(restored.zoom.center, axis.zoom.center);
        assert.ok(Math.abs(restored.pan - axis.pan) < 1e-12);
    }
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
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "144000", value: .5 }]);
    // A component remount only changes who reads this store.
    const remountedView = () => h.edits.drafts(target);
    h.edits.move(7, { x: 40, y: 25 });
    assert.deepEqual(remountedView(), [{ id: 1, seconds: 4, sampleIndex: 192000n, value: .75 }]);
    assert.deepEqual(h.edits.activePoint(), remountedView()[0]);
    const finished = h.edits.finish();
    assert.equal(h.edits.activePoint(), null);
    assert.equal(h.closed(), 0);
    release(); await finished;
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "144000", value: .5 },
        { id: 1, seconds: 0, sample_index: "192000", value: .75 }]);
    assert.equal(h.closed(), 1);
});

test("the released draft survives old reads and yields to the accepted revision including another client's newer edit", async () => {
    const h = harness(undefined, 8);
    h.edits.begin(target, 1, 7, axes);
    h.edits.move(7, { x: 40, y: 25 });
    await h.edits.finish();
    h.edits.reconcile(scope(7));
    assert.deepEqual(h.edits.drafts(target), [{ id: 1, seconds: 4, sampleIndex: 192000n, value: .75 }]);
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
    assert.deepEqual(h.edits.view(target)?.points.map(({ sampleIndex, ...point }) => point), construction.arguments[0]);
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
        { id: 1, seconds: 2, sampleIndex: 96000n, value: .5 },
        { id: 2, seconds: 7, sampleIndex: 336000n, value: .75 },
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
    assert.deepEqual((h.direct[1] as { payload: unknown }).payload,
        { point: { id: 3, seconds: 0, value: .25, interpolation: "bezier", tension: 1, sample_index: "144000" } });
});

test("creation and live rendering distinguish adjacent exact samples beyond the safe integer range", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    const rate = 48000, origin = -200_000_000_000;
    const base = BigInt(origin) * BigInt(rate);
    const spanSeconds = 10 / rate;
    const bounds = { ...axes, begin: origin, end: origin + spanSeconds, spanSeconds };
    const data = { ...construction, arguments: [[
        { ...construction.arguments[0][0], sample_index: String(base + 1n) },
        { ...construction.arguments[0][1], sample_index: String(base + 2n) },
    ]] };
    h.edits.reconcile(scope(2, data));
    assert.equal(h.edits.begin(target, 1, 7, bounds, { x: 10, y: 50 }), true);
    h.edits.move(7, { x: 14, y: 50 });
    assert.equal(h.edits.activePoint()?.sampleIndex, base + 1n);
    h.edits.move(7, { x: 16, y: 50 });
    assert.equal(h.edits.activePoint()?.sampleIndex, base + 2n);
    h.edits.move(7, { x: 26, y: 50 });
    const rendered = automationWindow(h.edits.view(target)!, 0, spanSeconds, h.edits.drafts(target), origin);
    assert.deepEqual(rendered.points.map((point) => point.sampleIndex), [base + 2n, base + 3n]);
    assert.ok(rendered.vertices.some((point) => point.seconds === 3 / rate && point.value === .5));
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, value: .5, sample_index: String(base + 3n) }]);
    await h.edits.add(target, curvePointAt(bounds, { x: 42, y: 50 }, rate));
    assert.deepEqual((h.direct[0] as { payload: { point: unknown } }).payload.point,
        { id: 3, seconds: 0, value: .5, interpolation: "bezier", tension: 1, sample_index: String(base + 4n) });
});

test("a tension drag updates its draft, clamps at both ends and closes one typed gesture", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); }, 4);
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }, "tension"), true);
    h.edits.move(7, { x: -20, y: 51 });
    assert.equal(h.edits.activePoint()?.tension, 0);
    h.edits.move(7, { x: 125, y: 70 });
    const [preview] = automationWindow(h.edits.view(target)!, 0, 4, h.edits.drafts(target)).points;
    assert.deepEqual([preview.seconds, preview.value, preview.tension], [1, .5, 1]);
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, interpolation: "bezier", tension: 1 }]);
    assert.equal(h.closed(), 1);
    assert.equal(h.edits.drafts(target)[0].tension, 1);
    h.edits.reconcile(scope(4));
    assert.deepEqual(h.edits.drafts(target), []);
});

test("automation labels share the timeline ticks and sample subdivisions follow the point transform", () => {
    const window = { beginSeconds: -.0002, endSeconds: .0002 };
    const rate = 48000, width = 400;
    const ticks = automationTimelineTicks(window, width, rate);
    assert.deepEqual(ticks.filter((tick) => !tick.minor).map(({ x, label }) => ({ x, label })),
        timelineTicks(window, width).map(({ x, label }) => ({ x, label })));
    assert.equal(ticks.find((tick) => tick.label === "0 s")?.sampleLabel, "0 smp");
    const expected = automationSampleOffset(1n, window.beginSeconds, rate) * width / .0004;
    assert.ok(ticks.some((tick) => tick.minor && Math.abs(tick.x - expected) < 1e-8));
    assert.ok(automationTimelineTicks({ beginSeconds: 0, endSeconds: 2 }, width, rate).every((tick) => !tick.minor));
});

test("optional automation bounds map independently and keep authored values outside the visible range", () => {
    for (const bounds of [{}, { min: -2 }, { max: 4 }, { min: -2, max: 4 }]) {
        for (const value of [-5, 0, 7]) {
            const fraction = valueScaleFraction(value, bounds);
            assert.ok(Math.abs(automationValueAtFraction(fraction, bounds) - value) < 1e-10);
            if (fraction >= 0 && fraction <= 1) assert.equal(fraction, knobFraction(value, bounds));
        }
    }
    assert.equal(valueScaleFraction(1, { min: -2, max: 4 }), .5);
    assert.ok(valueScaleFraction(7, { max: 4 }) > 1);
});

test("value panning shifts the center and keeps a one-sided bound anchored", () => {
    for (const bounds of [{}, { min: -2 }, { max: 4 }]) {
        const pan = panAutomationValueAxis(bounds, 25, 100);
        const axis = { ...bounds, pan };
        assert.ok(automationValueAtFraction(.5, axis) > automationValueAtFraction(.5, bounds));
        for (const fraction of [.01, .25, .5, .9, .99]) {
            const value = automationValueAtFraction(fraction, axis);
            assert.ok(Math.abs(automationValueFraction(value, axis) - fraction) < 1e-10);
        }
        if (bounds.min !== undefined) assert.equal(automationValueFraction(bounds.min, axis), 0);
        if (bounds.max !== undefined) assert.equal(automationValueFraction(bounds.max, axis), 1);
        const position = { x: 25, y: 25 };
        const point = curvePointAt({ ...axes, min: undefined, max: undefined, ...axis }, position, 48000);
        assert.ok(Math.abs(automationValueFraction(point.value, axis) - .75) < 1e-10);
    }
    assert.equal(panAutomationValueAxis({ min: -1, max: 1 }, 25, 100), 0);
});

test("the curve uses clamped points while stored values survive until dragged", async () => {
    const points = [{ ...construction.arguments[0][0], value: -2 }, { ...construction.arguments[0][1], value: 4 }];
    const settings = { min: { value: 0, enabled: true }, max: { value: 1, enabled: true } };
    const bounded = { ...construction, arguments: [points, settings] };
    const curve = readAutomationCurve(bounded)!;
    assert.deepEqual(automationWindow(curve, 1, 5).points.map(({ value }) => value), [-2, 4]);
    assert.equal(automationWindow(curve, 3, 4).vertices[0].value, .5);

    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    h.edits.reconcile(scope(2, bounded));
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 100 }), true);
    await h.edits.finish();
    assert.deepEqual(sent, []);
    assert.equal(h.edits.view(target)?.points[0].value, -2);

    assert.equal(h.edits.begin(target, 1, 8, axes, { x: 10, y: 100 }), true);
    h.edits.moveBy(8, { x: 10, y: 0 });
    assert.equal(h.edits.activePoint()?.value, 0);
    h.runTimer();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "96000", value: 0 }]);
    h.edits.moveBy(8, { x: 0, y: 300 });
    assert.equal(h.edits.activePoint()?.value, 0);
    h.edits.moveBy(8, { x: 0, y: -1 });
    assert.ok(h.edits.activePoint()!.value > 0);
    h.edits.moveBy(8, { x: 0, y: -300 });
    assert.equal(h.edits.activePoint()?.value, 1);
    await h.edits.finish();
    assert.deepEqual(sent[1], { id: 1, seconds: 0, sample_index: "96000", value: 1 });
    assert.deepEqual(h.edits.bounds(target), settings);
    await h.edits.add(target, { seconds: 2, value: 9 });
    assert.deepEqual((h.direct[0] as { payload: unknown }).payload,
        { point: { id: 3, seconds: 0, sample_index: "96000", value: 1, interpolation: "bezier", tension: 1 } });
});

test("moving past a disabled bound and re-enabling it preserves the opposite knob", async () => {
    const min = { value: .75, enabled: false };
    const initial = { min, max: { value: 1, enabled: true } };
    const withBounds = (bounds: typeof initial) => ({ ...construction, arguments: [construction.arguments[0], bounds] });
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); }, 5);
    h.edits.reconcile(scope(3, withBounds(initial)));
    assert.equal(h.edits.beginBound(target, "min", 7, { x: 0, y: 0 }, false), false);
    assert.equal(h.edits.beginBound(target, "max", 7, { x: 0, y: 0 }, false), true);
    h.edits.moveBy(7, { x: 0, y: 80 });
    const moved = h.edits.bounds(target)!;
    assert.ok(moved.max.value < min.value);
    assert.deepEqual(moved.min, min);
    assert.deepEqual(sent, []);
    await h.edits.finish();
    assert.deepEqual(sent, [{ side: "max", bound: moved.max }]);
    h.edits.reconcile(scope(4, withBounds(initial)));
    assert.deepEqual(h.edits.bounds(target), moved);
    h.edits.reconcile(scope(5, withBounds(moved)));
    await h.edits.setBound(target, "min", { ...min, enabled: true });
    const clamped = { value: moved.max.value, enabled: true };
    assert.deepEqual(sent[1], { side: "min", bound: clamped });
    assert.equal(h.closed(), 2);
    h.edits.reconcile(scope(6, withBounds({ ...moved, min: clamped })));
    assert.deepEqual(h.edits.bounds(target), { min: clamped, max: moved.max });
    assert.equal(h.edits.beginBound(target, "min", 8, { x: 0, y: 0 }, false), true);
    h.edits.moveBy(8, { x: 0, y: -80 });
    assert.equal(h.edits.activeBound()?.value, moved.max.value);
    h.edits.moveBy(8, { x: 0, y: 1 });
    assert.ok(h.edits.activeBound()!.value < moved.max.value);
    h.edits.abandon();
    assert.equal(sent.length, 2);
});

test("unbounded drags continue beyond their visible guides and approach the edge", async () => {
    const h = harness();
    const unbounded = { ...axes, min: undefined, max: undefined };
    assert.equal(h.edits.begin(target, 1, 7, unbounded, { x: 10, y: 35 }), true);
    h.edits.move(7, { x: 10, y: -65 });
    const upper = h.edits.activePoint()!;
    assert.ok(upper.value > 1);
    h.edits.move(7, { x: 10, y: -965 });
    const farther = h.edits.activePoint()!;
    assert.ok(knobFraction(farther.value, {}) > knobFraction(upper.value, {}));
    assert.ok(knobFraction(farther.value, {}) < 1);
    h.edits.move(7, { x: 10, y: 1035 });
    const lower = h.edits.activePoint()!;
    assert.ok(lower.value < -10);
    assert.ok(knobFraction(lower.value, {}) > 0);
    h.edits.move(7, { x: 10, y: 1034 });
    assert.ok(h.edits.activePoint()!.value > lower.value);
    assert.equal(h.edits.activePoint()!.sampleIndex, upper.sampleIndex);
    await h.edits.finish();
    assert.equal(h.closed(), 1);
});

test("creation at an unbounded edge maps to its nearest pixel centre", () => {
    assert.equal(curvePointAt(axes, { x: 10, y: -20 }).value, 1);
    assert.equal(curvePointAt({ ...axes, max: undefined }, { x: 10, y: 110 }).value, 0);
    const oneSided = { ...axes, min: undefined };
    const created = curvePointAt(oneSided, { x: 10, y: 110 }).value;
    assert.ok(Number.isFinite(created));
    assert.ok(Math.abs(knobFraction(created, oneSided) - .005) < 1e-12);
});

test("a horizontal drag preserves a point value close to the edge of an unbounded scale", async () => {
    const h = harness();
    const existing = structuredClone(construction);
    existing.arguments[0][0].value = 100;
    h.edits.reconcile(scope(2, existing));
    assert.equal(h.edits.begin(target, 1, 7, { ...axes, min: undefined, max: undefined }, { x: 10, y: 0 }), true);
    h.edits.move(7, { x: 11, y: 0 });
    assert.equal(h.edits.activePoint()!.value, 100);
    await h.edits.finish();
});

test("locked point drags use relative travel in both axes, even beyond the visible lane", async () => {
    const h = harness();
    const origin = { x: 10, y: 50 };
    const unbounded = { ...axes, min: undefined, max: undefined };
    assert.equal(h.edits.begin(target, 1, 7, unbounded, origin), true);
    let locked = false;
    let finished: Promise<void> | undefined;
    const mouse = new KnobPointerLock<Point>({
        request: () => {}, exit: () => { locked = false; }, locked: () => locked,
    }, (id, delta) => h.edits.moveBy(id, delta), (id, release, lockOrigin) => {
        if (release && !lockOrigin) h.edits.move(id, release);
        finished = h.edits.finish();
    }, (error) => assert.fail(error));
    mouse.start(7, origin);
    // An absolute move can arrive before the browser grants the lock.
    h.edits.move(7, { x: 11, y: 49 });
    locked = true;
    mouse.changed();
    mouse.move({ x: 9, y: -10000 });
    assert.ok(h.edits.activePoint()!.value > 300);
    assert.equal(h.edits.activePoint()!.sampleIndex, 96000n);
    mouse.move({ x: 0, y: 20000 });
    assert.ok(h.edits.activePoint()!.value < -300);
    mouse.release(7, origin);
    await finished;
    assert.equal(h.closed(), 1);
    assert.equal(locked, false);
});

test("locked tension drags use only horizontal travel and preserve it on release", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    const origin = { x: 10, y: 50 };
    assert.equal(h.edits.begin(target, 1, 7, axes, origin, "tension"), true);
    let locked = false;
    let finished: Promise<void> | undefined;
    const mouse = new KnobPointerLock<Point>({
        request: () => {}, exit: () => { locked = false; }, locked: () => locked,
    }, (id, delta) => h.edits.moveBy(id, delta), (id, release, lockOrigin) => {
        if (release && !lockOrigin) h.edits.move(id, release);
        finished = h.edits.finish();
    }, (error) => assert.fail(error));
    mouse.start(7, origin);
    h.edits.move(7, { x: 20, y: 49 });
    locked = true;
    mouse.changed();
    mouse.move({ x: 0, y: -10000 });
    assert.equal(h.edits.activePoint()?.tension, Math.fround(.1));
    mouse.move({ x: 140, y: 0 });
    assert.equal(h.edits.activePoint()?.tension, 1);
    mouse.move({ x: -75, y: 0 });
    assert.equal(h.edits.activePoint()?.tension, .75);
    mouse.release(7, { x: 9999, y: 9999 });
    await finished;
    assert.deepEqual(sent, [{ id: 1, interpolation: "bezier", tension: .75 }]);
    assert.equal(h.closed(), 1);
    assert.equal(locked, false);
});

test("configured display bounds never clamp the value authored by a point drag", async () => {
    const h = harness();
    for (const bounds of [{ min: 0 }, { max: 1 }, { min: 0, max: 1 }]) {
        const origin = { x: 10, y: 50 };
        assert.equal(h.edits.begin(target, 1, 7, { ...axes, min: undefined, max: undefined, ...bounds }, origin), true);
        h.edits.moveBy(7, { x: 0, y: -200 });
        assert.ok(h.edits.activePoint()!.value > 1);
        h.edits.moveBy(7, { x: 0, y: 400 });
        assert.ok(h.edits.activePoint()!.value < 0);
        await h.edits.finish();
    }
});

test("the wheel changes point value sensitivity without moving the point or changing time sensitivity", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    assert.equal(h.edits.changeSensitivity(-120), false);
    assert.equal(h.edits.begin(target, 1, 7, { ...axes, height: 64, ruler: "whole" }, { x: 10, y: 32 }), true);
    h.edits.moveBy(7, { x: 0, y: -8 });
    assert.equal(h.edits.activePoint()?.value, .625);
    assert.equal(h.edits.changeSensitivity(-120), true);
    assert.match(h.edits.sensitivityHint()!, /^2×/);
    assert.equal(h.edits.activePoint()?.value, .625);
    assert.equal(h.edits.activePoint()?.sampleIndex, 48000n);

    h.edits.moveBy(7, { x: 7, y: -8 });
    assert.equal(h.edits.activePoint()?.value, .875);
    assert.equal(h.edits.activePoint()?.sampleIndex, 81600n);
    h.edits.changeSensitivity(120);
    h.edits.changeSensitivity(120);
    assert.match(h.edits.sensitivityHint()!, /^0\.5×/);
    assert.equal(h.edits.activePoint()?.value, .875);
    h.edits.moveBy(7, { x: 0, y: 8 });
    assert.equal(h.edits.activePoint()?.value, .8125);
    await h.edits.finish();

    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "81600", value: .8125 }]);
    assert.equal(h.closed(), 1);
    assert.equal(h.edits.changeSensitivity(-120), false);
    assert.equal(h.edits.begin(target, 1, 8, axes, { x: 10, y: 50 }, "tension"), true);
    assert.equal(h.edits.changeSensitivity(-120), false);
    await h.edits.finish();
});

test("value guides use the curve scale, prioritize -1, 0, and 1, and thin crowded labels", () => {
    const guides = automationValueGuides({}, 240, "whole");
    assert.ok(guides.length > 3);
    assert.ok([-1, 0, 1].every((value) => guides.some((guide) => guide.value === value)));
    assert.ok(guides.every(({ value, y }) => Number.isInteger(value) && y === (1 - valueScaleFraction(value, {})) * 240));
    assert.ok(automationValueGuides({}, 30).length < guides.length);
    const bounded = automationValueGuides({ min: 11, max: 17 }, 160);
    assert.ok(bounded.length > 1 && bounded.every(({ value }) => value >= 11 && value <= 17));
});

test("the value ruler keeps adding finer powers of two where the scale has space", () => {
    const values = (height: number, ruler?: Parameters<typeof automationValueGuides>[2]) =>
        automationValueGuides({}, height, ruler).map(({ value }) => value);
    assert.ok(values(60).every(Number.isInteger));
    assert.ok(values(240).includes(.5));
    assert.ok([.25, .5, .75, 1.5].every((value) => values(600).includes(value)));
    const zoomedIn = values(1600);
    assert.ok([.125, .0625].every((value) => zoomedIn.includes(value)));
    assert.ok(automationValueGuides({ min: 0, max: .125 }, 480).some(({ value }) => value === 1 / 128));
    assert.ok(values(600, "half").includes(1.5));
    assert.ok(values(600, "half").every((value) => Number.isInteger(value * 2)));
    assert.deepEqual(values(600, "off"), []);
});

test("point drags snap to fractional values only when the ruler displays them", async () => {
    const h = harness();
    for (const ruler of ["auto", "whole", "off"] as const) {
        assert.equal(h.edits.begin(target, 1, 7, { ...axes, height: 160, ruler }, { x: 10, y: 80 }), true);
        h.edits.moveBy(7, { x: 0, y: -10 });
        assert.equal(h.edits.activePoint()?.value, ruler === "auto" ? .5 : .5625);
        await h.edits.finish();
    }
});

test("point drags snap to visible integers and retain their detent travel when sensitivity changes", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    const bounded = { ...axes, min: -2, max: 2 };
    assert.equal(h.edits.begin(target, 1, 7, bounded, { x: 10, y: 50 }), true);
    h.edits.moveBy(7, { x: 0, y: -20 });
    assert.equal(h.edits.activePoint()?.value, 1);
    h.edits.changeSensitivity(-1);
    h.edits.moveBy(7, { x: 5, y: -10 });
    assert.equal(h.edits.activePoint()?.value, 1);
    h.edits.moveBy(7, { x: 0, y: -10 });
    assert.equal(h.edits.activePoint()?.value, Math.fround(1.28));
    h.edits.moveBy(7, { x: 0, y: 100 });
    assert.equal(h.edits.activePoint()?.value, -1);
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "72000", value: -1 }]);
});

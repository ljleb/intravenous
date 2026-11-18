import { automationBoundTracks, oppositeAutomationBound, pairedAutomationBoundLimits,
    sampleAutomationPreview } from "../src/automationBoundInputs";
import test from "node:test";
import assert from "node:assert/strict";
import { AutomationCurveEditing, automationCycleSeconds, automationWindow, curvePointAt, readAutomationCurve, type CurveAxes } from "../src/automationCurveEditing";
import { automationSampleOffset, automationValueAtFraction, automationValueFraction, automationValueGuides,
    panAutomationValueAxis, readAutomationCurveView, zoomAutomationValueAxis } from "../src/automationCurveGeometry";
import type { GraphScope, GraphScopeNode, NodeAddress, Point } from "../src/graphScope";
import { KnobPointerLock } from "../src/knobPointerLock";
import { knobFraction, valueScaleFraction } from "../src/knobPresentation";
import { serializeNodeEvent, type NodeEvent } from "../src/nodeEvents";
import { wrapTimelineCursor } from "../src/timelineViewport";
import { automationTimelineTicks, timelineTicks } from "../webview/timeline/geometry";

const target: NodeAddress = { instanceId: "curve" };
const curvedAxes: CurveAxes = { left: 0, top: 0, width: 100, height: 100, begin: 0, end: 10 };
const axes: CurveAxes = { ...curvedAxes, display: "cropped", pan: .5, zoom: { factor: 2, center: .5 } };
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
    let changes = 0;
    const direct: unknown[] = [];
    const edits = new AutomationCurveEditing({ send: async (_target, event) => { direct.push(serializeNodeEvent(event)); },
        gesture: () => ({ send, finish: async () => { ++closed; }, acceptedRevision: () => revision }) }, () => { ++changes; },
        { set: (run) => { callback = run; return 1; }, clear: () => { callback = null; } });
    edits.reconcile(scope());
    return { edits, direct, runTimer: () => { const run = callback; callback = null; run?.(); },
        closed: () => closed, changes: () => changes };
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

test("cropped values remain linear as the window pans", () => {
    const axis = { pan: 2, display: "cropped" } as const;
    assert.equal(automationValueFraction(1, axis), 0);
    assert.equal(automationValueFraction(3, axis), 1);
    assert.equal(automationValueFraction(4, axis), 1.5);
    assert.equal(automationValueAtFraction(.25, axis), 1.5);
    assert.equal(panAutomationValueAxis(axis, 400, 100), 10);
    assert.equal(panAutomationValueAxis(axis, -400, 100), -6);
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
});

test("curved value zoom can display a wider range and preserve the anchor", () => {
    const anchor = .625;
    const axis = { pan: 1 };
    const value = automationValueAtFraction(anchor, axis);
    const zoomedOut = zoomAutomationValueAxis(axis, 1 / 256, anchor);
    assert.equal(zoomedOut.zoom.factor, 1 / 256);
    assert.ok(Math.abs(automationValueFraction(value, zoomedOut) - anchor) < 1e-10);
    assert.ok(Math.abs(automationValueAtFraction(.25, zoomedOut) - automationValueAtFraction(.75, zoomedOut)) > 500);
    const persisted = JSON.parse(JSON.stringify(readAutomationCurveView({ zoom: zoomedOut.zoom })));
    assert.deepEqual(readAutomationCurveView(persisted), { zoom: zoomedOut.zoom });
});

test("min and max clamp the preview independently of lane navigation", () => {
    const paths = [{ begin: { seconds: 0, value: -2 }, end: { seconds: 1, value: 4 }, first: 0, second: 2 }];
    const cases = [
        { bounds: {}, expected: [-2, 4] },
        { bounds: { min: 0 }, expected: [0, 4] },
        { bounds: { max: 1 }, expected: [-2, 1] },
        { bounds: { min: 0, max: 1 }, expected: [0, 1] },
    ];
    for (const { bounds, expected } of cases) {
        const preview = sampleAutomationPreview(paths, [], [], bounds, 1, 32);
        const [authored] = preview.authored, [output] = preview.output;
        assert.deepEqual([authored[0].value, authored.at(-1)?.value], [-2, 4]);
        assert.deepEqual([output[0].value, output.at(-1)?.value], expected);
        for (const display of ["curved", "cropped"] as const) {
            const axis = { display, pan: .5 };
            const combined = { ...axis, ...bounds };
            assert.equal(automationValueFraction(4, combined), automationValueFraction(4, axis));
            assert.equal(panAutomationValueAxis(combined, 150, 100), panAutomationValueAxis(axis, 150, 100));
            assert.deepEqual(zoomAutomationValueAxis(combined, .25, .75), zoomAutomationValueAxis(axis, .25, .75));
        }
    }
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
    assert.deepEqual(remountedView(), [{ ...construction.arguments[0][0], seconds: 4, sampleIndex: 192000n, value: .75 }]);
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
    assert.deepEqual(h.edits.drafts(target), [{ ...construction.arguments[0][0], seconds: 4, sampleIndex: 192000n, value: .75 }]);
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
        { ...construction.arguments[0][0], seconds: 2, sampleIndex: 96000n },
        { ...construction.arguments[0][1], seconds: 7, sampleIndex: 336000n, value: .75 },
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
});

test("a new point is sent immediately and follows the nearer gap edge while dragging", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    const data = { ...construction, arguments: [[
        { id: 1, seconds: 0, value: 1, interpolation: "hold", tension: 1 },
        { id: 2, seconds: 1, value: 1, interpolation: "empty", tension: .75 },
        { id: 3, seconds: 9, value: 0, interpolation: "bezier", tension: .4 },
        { id: 4, seconds: 10, value: 1, interpolation: "empty", tension: 1 },
    ]] };
    h.edits.reconcile(scope(2, data));
    assert.equal(h.edits.begin(target, null, 7, axes, { x: 20, y: 50 }), true);
    const point = { id: 5, seconds: 0, value: .5, tension: 1, interpolation: "empty", sample_index: "96000", order: 0 };
    assert.deepEqual(sent, [{ insert: true, point, segments: [{ id: 2, interpolation: "hold", tension: 1 }], ordered_ids: [] }]);
    h.edits.reconcile(scope(2, data));
    assert.equal(h.edits.pointerId(), 7);
    assert.ok(automationWindow(h.edits.view(target)!, 0, 10, h.edits.drafts(target)).points.some(({ id }) => id === 5));
    await new Promise<void>((resolve) => setImmediate(resolve));

    h.edits.moveBy(7, { x: 50, y: 0 });
    h.runTimer();
    assert.deepEqual(sent[1], { insert: false,
        point: { ...point, interpolation: "bezier", tension: .4, sample_index: "336000" },
        segments: [{ id: 2, interpolation: "empty", tension: .75 }], ordered_ids: [] });
    assert.equal(h.edits.drafts(target).find((draft) => draft.id === 2)?.interpolation, "empty");
    await h.edits.finish();
    assert.equal(h.closed(), 1);
    assert.equal(sent.length, 2);
});

test("a re-drag stays with its curve until it crosses the neighboring curve in either direction", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    h.edits.reconcile(scope(2, { ...construction, arguments: [[
        { id: 1, seconds: 0, value: 1, interpolation: "hold", tension: 1 },
        { id: 2, seconds: 2.5, value: 1, interpolation: "empty", tension: 1 },
        { id: 3, seconds: 10, value: 0, interpolation: "bezier", tension: .5 },
        { id: 4, seconds: 12, value: 0, interpolation: "empty", tension: 1 },
    ]] }));
    assert.equal(h.edits.begin(target, 2, 7, axes, { x: 25, y: 50 }), true);
    const style = (id: number) => (h.edits.drafts(target).find((point) => point.id === id)
        ?? h.edits.view(target)?.points.find((point) => point.id === id))?.interpolation;
    const move = (x: number) => h.edits.move(7, { x, y: 50 });
    move(75);
    assert.equal(style(1), "hold");
    assert.equal(style(2), "empty");
    move(104);
    assert.equal(h.edits.activePoint()?.seconds, 10);
    assert.equal(style(1), "hold");
    assert.equal(style(2), "empty");
    move(116);
    assert.equal(h.edits.activePoint()?.seconds, 10);
    assert.equal(style(1), "empty");
    assert.equal(style(2), "bezier");
    h.runTimer();
    const placed = { id: 2, seconds: 0, value: 1, sample_index: "480000", order: 1, interpolation: "bezier", tension: .5 };
    assert.deepEqual(sent[0], { insert: false, point: placed, ordered_ids: [3, 2],
        segments: [{ id: 1, interpolation: "empty", tension: 1 }] });
    await new Promise<void>((resolve) => setImmediate(resolve));
    move(104);
    assert.equal(h.edits.activePoint()?.seconds, 10);
    assert.equal(style(1), "empty");
    assert.equal(style(2), "bezier");
    move(25);
    assert.equal(h.edits.activePoint()?.seconds, 2.5);
    assert.equal(style(2), "bezier");
    move(-6);
    assert.equal(h.edits.activePoint()?.seconds, 0);
    assert.equal(style(2), "bezier");
    move(-16);
    assert.equal(h.edits.activePoint()?.seconds, 0);
    assert.equal(style(2), "hold");
    await h.edits.finish();
    assert.equal(h.closed(), 1);
});

test("Shift preserves the original pulse and tracks curve crossings until released", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    h.edits.reconcile(scope(2, { ...construction, arguments: [[
        { id: 1, seconds: 0, value: 1, interpolation: "hold", tension: 1 },
        { id: 2, seconds: 2.5, value: 1, interpolation: "empty", tension: 1 },
        { id: 3, seconds: 10, value: 0, interpolation: "bezier", tension: .5 },
        { id: 4, seconds: 12, value: 0, interpolation: "empty", tension: 1 },
    ]] }));
    assert.equal(h.edits.begin(target, 2, 7, axes, { x: 25, y: 50 }), true);
    h.edits.setPreservePointType(true);
    h.edits.move(7, { x: 116, y: 50 });
    h.edits.move(7, { x: 75, y: 50 });
    assert.equal(h.edits.activePoint()?.seconds, 7.5);
    assert.equal(h.edits.activePoint()?.interpolation, "empty");
    h.edits.setPreservePointType(false);
    assert.equal(h.edits.activePoint()?.seconds, 7.5);
    assert.equal(h.edits.drafts(target).find((point) => point.id === 1)?.interpolation, "empty");
    h.runTimer();
    const placed = { id: 2, seconds: 0, value: 1, sample_index: "360000", order: 0, interpolation: "bezier", tension: .5 };
    assert.deepEqual(sent[0], { insert: false, point: placed, ordered_ids: [],
        segments: [{ id: 1, interpolation: "empty", tension: 1 }] });
    await new Promise<void>((resolve) => setImmediate(resolve));
    const placement = h.edits.drafts(target);
    for (const shift of [true, false, true, false]) h.edits.setPreservePointType(shift);
    assert.deepEqual(h.edits.drafts(target), placement);
    h.edits.setPreservePointType(true);
    assert.equal(h.edits.activePoint()?.seconds, 7.5);
    assert.equal(h.edits.drafts(target).find((point) => point.id === 1)?.interpolation, "hold");
    await h.edits.finish();
    assert.deepEqual(sent[1], { insert: false, point: { ...placed, interpolation: "empty", tension: 1 }, ordered_ids: [],
        segments: [{ id: 1, interpolation: "hold", tension: 1 }] });
    assert.equal(sent.length, 2);
    assert.equal(h.closed(), 1);
});

test("Shift switches the interpolation inside a covered curve without moving the point", async () => {
    const h = harness();
    h.edits.reconcile(scope(2, { ...construction, arguments: [[
        ...construction.arguments[0].slice(0, 1),
        { id: 3, seconds: 3, value: .5, interpolation: "hold", tension: .75 },
        ...construction.arguments[0].slice(1),
    ]] }));
    assert.equal(h.edits.begin(target, 3, 7, axes, { x: 30, y: 50 }), true);
    h.edits.move(7, { x: 84, y: 50 });
    assert.equal(h.edits.activePoint()?.seconds, 6);
    assert.equal(h.edits.activePoint()?.interpolation, "bezier");
    h.edits.setPreservePointType(true);
    assert.equal(h.edits.activePoint()?.seconds, 6);
    assert.equal(h.edits.activePoint()?.interpolation, "hold");
    assert.equal(h.edits.activePoint()?.tension, .75);
    h.edits.setPreservePointType(false);
    assert.equal(h.edits.activePoint()?.interpolation, "bezier");
    await h.edits.finish();
});

test("both drag modes choose the order on either half of the same sample detent", async () => {
    for (const shift of [false, true]) {
        const sent: Array<{ ordered_ids?: number[] }> = [];
        const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload as typeof sent[number]); }, 3);
        const points = [...construction.arguments[0], { ...construction.arguments[0][1], id: 3, value: .25, order: 0 }];
        const data = { ...construction, arguments: [[points[0], { ...points[1], order: 1 }, points[2]]] };
        h.edits.reconcile(scope(2, data));
        const ids = () => automationWindow(h.edits.view(target)!, 0, 10, h.edits.drafts(target)).points.map(({ id }) => id);
        assert.deepEqual(ids(), [1, 3, 2]);
        assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }, "point", shift), true);
        h.edits.move(7, { x: 54, y: 50 });
        assert.equal(h.edits.activePoint()?.sampleIndex, 240000n);
        assert.deepEqual(ids(), [1, 3, 2]);
        h.runTimer();
        await new Promise<void>((resolve) => setImmediate(resolve));
        h.edits.move(7, { x: 66, y: 50 });
        assert.equal(h.edits.activePoint()?.sampleIndex, 240000n);
        assert.deepEqual(ids(), [3, 2, 1]);
        h.edits.setPreservePointType(!shift);
        assert.deepEqual(ids(), [3, 2, 1]);
        h.edits.setPreservePointType(shift);
        h.edits.move(7, { x: 78, y: 50 });
        assert.equal(h.edits.activePoint()?.seconds, 5.4);
        h.edits.move(7, { x: 54, y: 50 });
        assert.deepEqual(ids(), [1, 3, 2]);
        h.edits.move(7, { x: 66, y: 50 });
        await h.edits.finish();
        assert.deepEqual(sent.map(({ ordered_ids }) => ordered_ids), [[1, 3, 2], [3, 2, 1]]);
        assert.deepEqual(ids(), [3, 2, 1]);
        h.edits.reconcile(scope(3, { ...data, arguments: [[{ ...points[0], seconds: 5, order: 2 }, points[2], { ...points[1], order: 1 }]] }));
        assert.deepEqual(ids(), [3, 2, 1]);
        assert.equal(h.closed(), 1);
    }
});

test("a right press on a blank curve adds an empty point, even without motion", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    h.edits.reconcile(scope(2, { ...construction, arguments: [[]] }));
    assert.equal(h.edits.begin(target, null, 7, axes, { x: 10, y: 50 }), true);
    assert.deepEqual(sent, [{ insert: true,
        point: { id: 0, seconds: 0, value: .5, interpolation: "empty", tension: 1, sample_index: "48000", order: 0 },
        segments: [], ordered_ids: [] }]);
    await h.edits.finish();
    assert.equal(sent.length, 1);
    assert.equal(h.closed(), 1);
});

test("Shift-click still deletes while motion moves the point with its original type", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }, "point", true), true);
    h.edits.moveBy(7, { x: 1, y: 0 });
    await h.edits.finish(true);
    assert.deepEqual(sent, [{ id: 1 }]);

    assert.equal(h.edits.begin(target, 2, 7, axes, { x: 50, y: 0 }, "point", true), true);
    h.edits.moveBy(7, { x: 10, y: 0 });
    await h.edits.finish(true);
    assert.deepEqual(sent[1], { id: 2, seconds: 0, value: 1, sample_index: "288000" });
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
    assert.equal(h.edits.begin(target, 1, 7, bounds, { x: 10, y: 50 }, "point", true), true);
    h.edits.move(7, { x: 14, y: 50 });
    assert.equal(h.edits.activePoint()?.sampleIndex, base + 1n);
    h.edits.move(7, { x: 16, y: 50 });
    assert.equal(h.edits.activePoint()?.sampleIndex, base + 2n);
    h.edits.move(7, { x: 56, y: 50 });
    const rendered = automationWindow(h.edits.view(target)!, 0, spanSeconds, h.edits.drafts(target), origin);
    assert.deepEqual(rendered.points.map((point) => point.sampleIndex), [base + 2n, base + 3n]);
    assert.ok(rendered.vertices.some((point) => point.seconds === 3 / rate && point.value === .5));
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, value: .5, sample_index: String(base + 3n) }]);
    assert.equal(h.edits.begin(target, null, 7, bounds, { x: 42, y: 50 }), true);
    await h.edits.finish();
    assert.deepEqual(sent[1], { insert: true, segments: [], ordered_ids: [],
        point: { id: 3, seconds: 0, value: .5, interpolation: "bezier", tension: 0, sample_index: String(base + 4n), order: 0 } });
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

test("value panning shifts the center in either display mode", () => {
    for (const display of ["curved", "cropped"] as const) {
        const pan = panAutomationValueAxis({ display }, 25, 100);
        const axis = { ...curvedAxes, display, pan };
        assert.equal(automationValueAtFraction(.5, axis), pan);
        assert.ok(pan > 0);
        for (const fraction of [.01, .25, .5, .9, .99]) {
            const value = automationValueAtFraction(fraction, axis);
            assert.ok(Math.abs(automationValueFraction(value, axis) - fraction) < 1e-10);
        }
        const position = { x: 25, y: 25 };
        const point = curvePointAt(axis, position, 48000);
        assert.ok(Math.abs(automationValueFraction(point.value, axis) - .75) < 1e-10);
    }
});

test("output bounds leave interpolation and point edits in the authored domain", async () => {
    const points = [{ ...construction.arguments[0][0], value: -2 }, { ...construction.arguments[0][1], value: 4 }];
    const bounded = { ...construction, arguments: [points] };
    const curve = readAutomationCurve(bounded)!;
    assert.deepEqual(automationWindow(curve, 1, 5).points.map(({ value }) => value), [-2, 4]);
    assert.equal(automationWindow(curve, 4, 4.5).vertices[0].value, 2.5);

    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    h.edits.reconcile(scope(2, bounded));
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 100 }), true);
    h.edits.moveBy(7, { x: 10, y: 0 });
    assert.equal(h.edits.activePoint()?.value, -2);
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "96000", value: -2 }]);
});

test("an automation input respects an enabled opposite knob", () => {
    const opposite = oppositeAutomationBound(construction.definitionId, "min");
    assert.equal(opposite, "max");
    assert.deepEqual(pairedAutomationBoundLimits(opposite, { enabled: false, value: .5 }), {});
    assert.deepEqual(pairedAutomationBoundLimits(opposite, { enabled: true, value: .5 }), { max: .5 });
    assert.deepEqual(pairedAutomationBoundLimits("min", { enabled: true, value: .5, liveValue: .25 }), { min: .25 });
    assert.equal(oppositeAutomationBound("iv.other", "min"), undefined);
});

test("bound tracks keep unavailable samples and separate Coverage Regions out of the preview", () => {
    const tracks = automationBoundTracks({ id: "max", status: "ready", approximate: true,
        window: { begin: "-2", length: "8", points: 8 }, regions: [
            { begin: "-2", length: "5", points: [
                { index: "18446744073709551614", available: true, value: { value: 0 } },
                { index: "-1", available: true, value: { value: 2 } },
                { index: "0", available: false },
                { index: "1", available: true, value: { value: 4 } },
            ] },
            { begin: "4", length: "2", points: [{ index: "4", available: true, value: { value: 8 } }] },
        ] }, -2, 1);
    const preview = sampleAutomationPreview([], [], tracks, {}, 8, 16);
    assert.deepEqual(preview.max.map((run) => [run[0].seconds, run.at(-1)?.seconds]), [[0, 2], [3, 5], [6, 8]]);
    assert.equal(preview.max[0].find(({ seconds }) => seconds === .5)?.value, 1);
    assert.equal(preview.max[1][0].value, 4);
    assert.equal(preview.max[2][0].value, 8);
});

test("the automation preview combines bounds with the authored curve before projection", () => {
    const tracks = automationBoundTracks({ id: "min", status: "ready", approximate: true,
        window: { begin: "0", length: "8", points: 8 }, regions: [
            { begin: "1", length: "3", points: [
                { index: "1", available: true, value: { value: 10 } },
                { index: "3", available: true, value: { value: 14 } },
            ] },
            { begin: "2", length: "1", points: [{ index: "2", available: true, value: { value: 30 } }] },
        ] }, 0, 1);
    const paths = [{ begin: { seconds: 0, value: 0 }, end: { seconds: 6, value: 6 }, first: 2, second: 4 }];
    const preview = sampleAutomationPreview(paths, tracks, [], { max: 4 }, 8, 16);
    assert.equal(preview.min[0][0].value, 10);
    assert.equal(preview.min[0].find(({ seconds }) => seconds === 1.5)?.value, 11);
    assert.equal(preview.min[1][0].value, 30);
    assert.deepEqual(preview.output.map((run) => [run[0].seconds, run.at(-1)?.seconds]),
        [[0, 1], [1, 2], [2, 3], [3, 4], [4, 6]]);
    assert.equal(preview.output[1].find(({ seconds }) => seconds === 1.5)?.value, 7.5);
    assert.equal(preview.output[2][0].value, 17);
    assert.equal(preview.output[4].at(-1)?.value, 4);
    assert.equal(preview.authored.length, 1);
    assert.equal(preview.authored[0].at(-1)?.value, 6);
});

test("unbounded drags continue beyond their visible guides and approach the edge", async () => {
    const h = harness();
    assert.equal(h.edits.begin(target, 1, 7, curvedAxes, { x: 10, y: 35 }), true);
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

test("creation at a curved edge maps to its nearest pixel center", () => {
    assert.equal(curvePointAt(axes, { x: 10, y: -20 }).value, 1);
    assert.equal(curvePointAt(axes, { x: 10, y: 110 }).value, 0);
    for (const [y, fraction] of [[-20, .995], [110, .005]]) {
        const { value } = curvePointAt(curvedAxes, { x: 10, y });
        assert.ok(Number.isFinite(value));
        assert.ok(Math.abs(automationValueFraction(value, curvedAxes) - fraction) < 1e-12);
    }
});

test("a horizontal drag preserves a point value close to the edge of an unbounded scale", async () => {
    const h = harness();
    const existing = structuredClone(construction);
    existing.arguments[0][0].value = 100;
    h.edits.reconcile(scope(2, existing));
    assert.equal(h.edits.begin(target, 1, 7, curvedAxes, { x: 10, y: 0 }), true);
    h.edits.move(7, { x: 11, y: 0 });
    assert.equal(h.edits.activePoint()!.value, 100);
    await h.edits.finish();
});

test("locked point drags use relative travel in both axes, even beyond the visible lane", async () => {
    const h = harness();
    const origin = { x: 10, y: 50 };
    assert.equal(h.edits.begin(target, 1, 7, curvedAxes, origin), true);
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

test("the tension knob scrubs vertically and changes sensitivity without jumping", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    assert.equal(h.edits.begin(target, 1, 7, axes, { x: 10, y: 50 }, "tensionKnob"), true);
    h.edits.moveBy(7, { x: 50, y: 0 });
    assert.equal(h.edits.activePoint()?.tension, 0);
    h.edits.moveBy(7, { x: 0, y: -50 });
    assert.equal(h.edits.activePoint()?.tension, .25);
    assert.equal(h.edits.changeSensitivity(-120), true);
    assert.equal(h.edits.activePoint()?.tension, .25);
    h.edits.moveBy(7, { x: 0, y: -100 });
    assert.equal(h.edits.activePoint()?.tension, 1);
    h.edits.moveBy(7, { x: 0, y: 25 });
    assert.equal(h.edits.activePoint()?.tension, .75);
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, interpolation: "bezier", tension: .75 }]);
    assert.equal(h.closed(), 1);
});

test("point drags can move past the visible value window", async () => {
    const h = harness();
    for (const axis of [axes, curvedAxes]) {
        assert.equal(h.edits.begin(target, 1, 7, axis, { x: 10, y: 50 }), true);
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
    const cropped = automationValueGuides({ display: "cropped", pan: 14, zoom: { factor: 1 / 3, center: .5 } }, 160);
    assert.ok(cropped.length > 1 && cropped.every(({ value }) => value >= 11 && value <= 17));
});

test("the value ruler keeps adding finer powers of two where the scale has space", () => {
    const values = (height: number, ruler?: Parameters<typeof automationValueGuides>[2]) =>
        automationValueGuides({}, height, ruler).map(({ value }) => value);
    assert.ok(values(60).every(Number.isInteger));
    assert.ok(values(240).includes(.5));
    assert.ok([.25, .5, .75, 1.5].every((value) => values(600).includes(value)));
    const zoomedIn = values(1600);
    assert.ok([.125, .0625].every((value) => zoomedIn.includes(value)));
    const cropped = { display: "cropped", pan: .0625, zoom: { factor: 16, center: .5 } } as const;
    assert.ok(automationValueGuides(cropped, 480).some(({ value }) => value === 1 / 128));
    assert.ok(values(600, "half").includes(1.5));
    assert.ok(values(600, "half").every((value) => Number.isInteger(value * 2)));
    assert.deepEqual(values(600, "off"), []);
});

test("point drags snap to fractional values only when the ruler displays them", async () => {
    const h = harness();
    for (const ruler of ["auto", "whole", "off"] as const) {
        assert.equal(h.edits.begin(target, 1, 7, { ...axes, height: 160, ruler }, { x: 10, y: 80 }), true);
        assert.equal(h.edits.activeSnap(target), null);
        const changes = h.changes();
        h.edits.moveBy(7, { x: 0, y: -10 });
        assert.equal(h.edits.activePoint()?.value, ruler === "auto" ? .5 : .5625);
        assert.equal(h.edits.activeSnap(target)?.value, ruler === "auto" ? .5 : undefined);
        assert.equal(h.edits.activeSnap({ instanceId: "other" }), null);
        assert.equal(h.changes(), changes + 1);
        const finished = h.edits.finish();
        assert.equal(h.edits.activeSnap(target), null);
        await finished;
    }
});

test("point drags snap to visible integers and retain their detent travel when sensitivity changes", async () => {
    const sent: unknown[] = [];
    const h = harness(async (event) => { sent.push(serializeNodeEvent(event).payload); });
    const wideView = { ...axes, pan: 0, zoom: { factor: .5, center: .5 } };
    assert.equal(h.edits.begin(target, 1, 7, wideView, { x: 10, y: 50 }), true);
    h.edits.moveBy(7, { x: 0, y: -20 });
    assert.equal(h.edits.activePoint()?.value, 1);
    assert.equal(h.edits.activeSnap(target)?.value, 1);
    h.edits.changeSensitivity(-1);
    h.edits.moveBy(7, { x: 5, y: -10 });
    assert.equal(h.edits.activePoint()?.value, 1);
    assert.equal(h.edits.activeSnap(target)?.value, 1);
    h.edits.moveBy(7, { x: 0, y: -10 });
    assert.equal(h.edits.activePoint()?.value, Math.fround(1.28));
    assert.equal(h.edits.activeSnap(target), null);
    h.edits.moveBy(7, { x: 0, y: 100 });
    assert.equal(h.edits.activePoint()?.value, -1);
    assert.equal(h.edits.activeSnap(target)?.value, -1);
    await h.edits.finish();
    assert.deepEqual(sent, [{ id: 1, seconds: 0, sample_index: "72000", value: -1 }]);
});

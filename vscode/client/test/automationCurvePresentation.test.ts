import assert from "node:assert/strict";
import { test, type TestContext } from "node:test";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { createWasmPresentation, readWasmPresentationDescriptor, wasmPresentationUpdate } from "../src/wasmPresentation";
import { renderPresentationSecondsRuler } from "../src/presentationSecondsRuler";
import { projectTimelineCurveFrame } from "../src/timelineCurvePresentation";
import { PresentationLaneEdits } from "../src/presentationLaneEdits";
import { parsePresentationState } from "../src/presentationStateJson";
import type { ObservationWindowResult } from "../src/observations";
import { readPresentationFrame, type PresentationCommand, type PresentationFrame, type PresentationGestureInput,
    type PresentationInput, type PresentationObservation, type PresentationSnapshot } from "../src/presentationPlugin";

const points = [
    { id: 1, seconds: 1, value: .5, interpolation: "bezier", tension: 1, sample_index: "", order: 0 },
    { id: 2, seconds: 5, value: 1, interpolation: "empty", tension: 1, sample_index: "", order: 0 },
];
const snapshot: PresentationSnapshot = {
    node: { definitionId: "builtin.automation.curve", constructionDefinitionId: "builtin.automation.curve", address: { instanceId: "curve" }, arguments: [points], ports: [] },
    mode: "editable", editable: true, clockMs: 0, revision: 1, state: null, observations: {},
    viewport: { width: 100, height: 100, time: { beginSample: "0", beginFraction: 0, spanSamples: 480000, sampleRate: 48000 } },
};
const modifiers = { shift: false, alt: false, control: false, meta: false };
function gesture(id: string, phase: PresentationGestureInput["phase"], x: number, y: number,
    options?: Partial<PresentationGestureInput>): PresentationGestureInput {
    return { type: "gesture", id, phase, point: { x, y }, modifiers, ...options };
}
const artifact = readFileSync(resolve(__dirname, "../presentations/automation_curve.wasm"));
const selectionTarget = "1", addTarget = "2", firstPoint = "3", firstTension = "4";
const dotId = (handle: number) => String(0x40000000 + 2 * handle + 1);
const ringId = (handle: number) => String(0x40000000 + 2 * handle);
const tracks = { authored: 1001, output: 1301, min: 1601, max: 1901 };

function harness(t: TestContext, initial = snapshot) {
    const present = createWasmPresentation(artifact.toString("base64"));
    present.restore(initial.state ?? "");
    t.after(() => present.dispose());
    let argumentsInGuest: readonly unknown[] | undefined;
    return (inputs: PresentationInput[] = [], update?: Partial<PresentationSnapshot>) => {
        const current = { ...initial, ...update };
        const input = wasmPresentationUpdate(current, inputs);
        if (input.construction?.arguments === argumentsInGuest) delete input.construction;
        else argumentsInGuest = input.construction?.arguments;
        const reply = present.present(input);
        const frame = projectTimelineCurveFrame(readPresentationFrame(reply.frame), current);
        return { ...frame, commands: frame.commands ?? [] };
    };
}

function dot(frame: PresentationFrame, id: number) {
    const mark = frame.marks?.find((item) => item.id === dotId(id - 1));
    assert.equal(mark?.type, "circle");
    return mark;
}

function trace(frame: PresentationFrame, firstId: number) {
    return frame.marks?.flatMap((mark) => {
        const id = Number(mark.id);
        if (mark.type !== "packedPolyline" || id < firstId || id >= firstId + 256) return [];
        const run = Array.from({ length: mark.coordinates.length / 2 }, (_, i) =>
            ({ x: mark.coordinates[i * 2], y: mark.coordinates[i * 2 + 1] }));
        return [run];
    }) ?? [];
}

function acknowledge(command: PresentationCommand | undefined, acceptedRevision: number): PresentationInput {
    assert.ok(command && "requestId" in command && command.requestId);
    return { type: "editResult", requestId: command.requestId, acceptedRevision };
}

test("curve gestures keep exact playhead detents, send drafts before release and close after acknowledgement", (t) => {
    const present = harness(t);
    const point = dot(present(), 1);
    present([gesture(firstPoint, "begin", point.x, point.y, { snapping: { marks: [], playhead: "96000" } })]);
    const moving = present([gesture(firstPoint, "move", 20, point.y)], { clockMs: 10 });
    assert.equal(dot(moving, 1).x, 20);
    assert.ok(moving.marks?.some(({ id }) => id === "404"));
    assert.deepEqual(moving.commands, []);
    assert.equal(moving.wakeAfterMs, 80);
    const paced = present([{ type: "wake" }], { clockMs: 90 });
    const move = paced.commands?.[0];
    assert.equal(move?.type, "edit");
    assert.deepEqual(move.event.payload, { id: 1, seconds: 0, sample_index: "96000", value: .5 });

    const release = present([gesture(firstPoint, "finish", 20, point.y)], { clockMs: 95 });
    assert.deepEqual(release.drag?.cursor, { sample: "96000", yFraction: point.y / snapshot.viewport.height });
    assert.deepEqual(release.commands, []);
    const finish = present([acknowledge(move, 2)]).commands?.[0];
    assert.equal(finish?.type, "finish");
    assert.equal(finish.gesture, move.gesture);
    present([acknowledge(finish, 2)]);
    const held = present();
    assert.equal(dot(held, 1).x, 20);
    assert.equal(held.drag, undefined);
    assert.equal(held.marks?.some(({ id }) => id === ringId(0)), false);
    const published = present([], { revision: 2, node: { ...snapshot.node,
        arguments: [[{ ...points[0], seconds: 2, sample_index: "96000" }, points[1]]] } });
    assert.equal(dot(published, 1).x, 20);
});

test("the curve flushes a new point immediately and history discards an unsent move", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, arguments: [[]] } });
    const added = present([gesture(addTarget, "begin", 20, 50)]);
    const insert = added.commands?.[0];
    assert.equal(insert?.type, "edit");
    assert.deepEqual(insert.event.payload, { insert: true, ordered_ids: [], segments: [],
        point: { id: 0, seconds: 0, sample_index: "96000", value: 0, interpolation: "empty", tension: 1, order: 0 } });
    present([gesture(addTarget, "move", 30, 40)], { clockMs: 10 });
    const interrupted = present([{ type: "interrupt", reason: "history" }], { clockMs: 100 });
    assert.equal(interrupted.cancelDrag, true);
    assert.deepEqual(interrupted.commands, []);
    const closing = present([acknowledge(insert, 2)], { clockMs: 101 });
    assert.deepEqual(closing.commands?.map(({ type }) => type), ["finish"]);
});

test("history settles the curve's finish request while new edits are blocked", async (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, arguments: [[]] } });
    let editable = true;
    let sends = 0;
    let finishes = 0;
    let completeSend!: () => void;
    const pendingSend = new Promise<void>((resolve) => { completeSend = resolve; });
    const replies: PresentationInput[] = [];
    const edits = new PresentationLaneEdits({
        editable: () => editable,
        gesture: () => ({
            send: async () => { ++sends; await pendingSend; },
            finish: async () => { ++finishes; },
            acceptedRevision: () => 2,
        }),
        accepted: () => {}, input: (reply) => replies.push(reply), setError: () => {}, changed: () => {},
        fail: (error) => assert.fail(String(error)),
    });
    t.after(() => { completeSend(); edits.queue.dispose(); return edits.closeGestures(); });
    const added = present([gesture(addTarget, "begin", 20, 50)]);
    const insert = added.commands?.[0];
    assert.equal(insert?.type, "edit");
    edits.accept(added.commands);
    assert.equal(sends, 1);

    editable = false;
    const closing = present([{ type: "interrupt", reason: "history" }, ...edits.interrupt()], { editable });
    const finish = closing.commands?.[0];
    assert.equal(finish?.type, "finish");
    assert.ok(finish.requestId);
    assert.equal(finishes, 1);
    edits.accept(closing.commands, false);
    edits.accept([{ ...insert, requestId: "blocked-edit" }], false);
    assert.deepEqual(replies.splice(0), [{ type: "editResult", requestId: "blocked-edit", error: "This frame cannot submit node edits" }]);
    completeSend();
    await new Promise<void>((resolve) => setImmediate(resolve));
    assert.equal(sends, 1);
    assert.deepEqual(replies, [{ type: "editResult", requestId: finish.requestId, acceptedRevision: undefined }]);
    present(replies.splice(0), { editable });

    editable = true;
    const resumed = present([gesture(addTarget, "begin", 40, 50)], { editable, revision: 3 });
    assert.equal(resumed.commands?.[0]?.type, "edit");
    assert.equal(resumed.cancelDrag, undefined);
    edits.accept(resumed.commands);
    assert.equal(sends, 2);
});

test("rectangle selection and contextual controls use the same frame as the curve", (t) => {
    const present = harness(t);
    const first = present();
    assert.equal(first.targets?.find(({ id }) => id === selectionTarget)?.drag, "absolute");
    assert.equal(first.targets?.find(({ id }) => id === addTarget)?.button, "secondary");
    const selection = present([gesture(selectionTarget, "begin", 0, 13), gesture(selectionTarget, "finish", 70, 80)]);
    assert.ok(selection.marks?.some(({ id }) => id === ringId(0)));
    assert.ok(selection.marks?.some(({ id }) => id === ringId(1)));
    const toggle = { modifiers: { ...modifiers, control: true } };
    const second = present([gesture(selectionTarget, "begin", 3, 16, toggle), gesture(selectionTarget, "finish", 19, 80, toggle),
        { type: "context", id: firstTension, point: { x: 10, y: 45 } }]);
    assert.equal(second.marks?.some(({ id }) => id === ringId(0)), false);
    assert.ok(second.marks?.some(({ id }) => id === ringId(1)));
    const knob = second.contextMenu?.rows.flat().find(({ id }) => id === "tension");
    assert.equal(knob?.type, "knob");
    assert.equal(knob.value, 1);
    const command = present([{ type: "items", action: "delete" }]).commands?.[0];
    assert.equal(command?.type, "edit");
    assert.deepEqual(command.event.payload, { id: 2 });
});

test("scale commands save the curve view and a reloaded lane draws it over a manual bound", (t) => {
    const present = harness(t);
    const scale = present([{ type: "control", id: "display", phase: "commit", value: "cropped" },
        { type: "valueView", action: "zoom", factor: 2, y: .5 }]);
    assert.ok(scale.saveRequest);
    assert.equal(parsePresentationState(scale.saveRequest)?.display, "cropped");
    const restored = harness(t, { ...snapshot, state: scale.saveRequest, node: { ...snapshot.node,
        ports: [{ name: "min", kind: "continuous", direction: "input", manualValue: .8 }] } });
    const frame = restored();
    assert.ok(trace(frame, tracks.min).length);
    assert.ok(trace(frame, tracks.output).length);
    const reset = restored([{ type: "valueView", action: "reset" }]);
    assert.deepEqual(parsePresentationState(reset.saveRequest!), { display: "cropped" });
});

test("a connected bound uses the retained spans while its replacement is pending", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node,
        ports: [{ name: "max", kind: "continuous", direction: "input", connected: true, manualValue: -1 }] } });
    const result: ObservationWindowResult = {
        id: "max", status: "ready", approximate: false, window: { begin: "0", length: "480000", points: 1024 },
        regions: [{ begin: "0", length: "480000", points: [
            { index: "0", available: true, value: { value: 0 } },
            { index: "479999", available: true, value: { value: 1 } },
        ] }],
    };
    const spans = [{ begin: "0", end: "480000", windowOffset: "0", result }];
    const maxTrace = (frame: PresentationFrame) => {
        const [run] = trace(frame, tracks.max);
        assert.ok(run?.length);
        return run;
    };
    const first = maxTrace(present([], { observations: { max: { operation: "waveform", beginSample: "0", status: "ready", result, spans } } }));
    assert.equal(first[0].y, 50);
    assert.equal(first.at(-1)?.y, 25);
    const pending = { ...result, status: "pending" as const, regions: [] };
    assert.deepEqual(maxTrace(present([], { observations: { max: { operation: "waveform", beginSample: "0", status: "pending", result: pending, spans } } })), first);
    const panned = present([], { viewport: { ...snapshot.viewport,
        time: { ...snapshot.viewport.time!, beginSample: "240000" } },
        observations: { max: { operation: "waveform", beginSample: "0", status: "pending", result: pending, spans } } });
    const run = maxTrace(panned);
    assert.equal(run[0].x, 0);
    assert.ok(Math.abs(run[0].y - (1 - (.5 + Math.atan(240000 / 479999) / Math.PI)) * 100) < 1e-9);
    assert.equal(run.at(-1)?.x, 50);
});

test("automation points and interpolation follow the same view through the signed seam", (t) => {
    const seam = 1n << 63n;
    const pointsAcross = [
        { ...points[0], id: 21, sample_index: String(seam - 2n), value: 0, interpolation: "bezier", tension: 0 },
        { ...points[0], id: 22, sample_index: String(-seam + 2n), value: 1, interpolation: "empty" },
    ];
    const time = { beginSample: String(seam - 5n), beginFraction: 0, spanSamples: 10, sampleRate: 48000 };
    const present = harness(t, { ...snapshot, viewport: { ...snapshot.viewport, time },
        node: { ...snapshot.node, arguments: [pointsAcross] } });
    const frame = present();
    const first = frame.targets?.find(({ hint }) => hint?.startsWith(`${seam - 2n} smp`));
    const second = frame.targets?.find(({ hint }) => hint?.startsWith(`${-seam + 2n} smp`));
    assert.ok(first && second);
    assert.ok(first.x < 50 && second.x > 50);
    const traces = trace(frame, tracks.authored);
    assert.ok(Math.abs(traces[0][0].x - 30) < 1e-9);
    assert.ok(Math.abs(traces.at(-1)!.at(-1)!.x - 70) < 1e-9);
    const midpoint = traces.flat().filter(({ x }) => Math.abs(x - 50) < 1e-9);
    assert.ok(midpoint.length && midpoint.every(({ y }) => Math.abs(y - midpoint[0].y) < 1e-9));
});

test("a densely authored curve fits the bounded frame and retains points across the viewport", (t) => {
    const dense = points[0];
    const present = harness(t, { ...snapshot, viewport: { ...snapshot.viewport, width: 4000 },
        node: { ...snapshot.node, arguments: [Array.from({ length: 7000 }, (_, i) => ({
            ...dense, id: i, seconds: 10 * i / 7000, value: i % 2, interpolation: i % 3 === 0 ? "empty" : "bezier",
        }))], ports: [{ name: "min", direction: "input", kind: "continuous", manualValue: .1 },
            { name: "max", direction: "input", kind: "continuous", manualValue: .5 }] },
    });
    const frame = present();
    assert.ok(frame.targets?.some(({ id, x }) => Number(id) >= 3 && Number(id) % 2 === 1 && x > 3900));
    assert.ok(frame.targets?.some(({ id, x }) => Number(id) >= 3 && Number(id) % 2 === 1 && x < 100));
});


test("the published Automation Curve requests timeline snapping and suppresses seconds labels in custom grid and nano mode", (t) => {
    assert.deepEqual(readWasmPresentationDescriptor(artifact), { timeline: true, timelineSnapping: true, icon: "M2 12C5 12 5 4 14 4M2 10v4m12-12v4" });
    const frame = harness(t)();
    const view = snapshot.viewport.time!;
    assert.ok(frame.secondsRulerInset! > 0);
    assert.ok(renderPresentationSecondsRuler(view, 100, 100, "editable", frame.secondsRulerInset!).some((mark) => mark.type === "text"));
    assert.ok(renderPresentationSecondsRuler(view, 100, 100, "nano", frame.secondsRulerInset!).every((mark) => mark.type !== "text"));
    assert.deepEqual(renderPresentationSecondsRuler({ ...view, gridMode: "custom" }, 100, 100, "editable", frame.secondsRulerInset!), []);
});

test("a group drag sends each selected point in one edit gesture and cancels a removed point", (t) => {
    const present = harness(t);
    present([gesture(selectionTarget, "begin", 0, 25), gesture(selectionTarget, "finish", 65, 80)]);
    const point = dot(present(), 1);
    present([gesture(firstPoint, "begin", point.x, point.y)]);
    const drag = present([gesture(firstPoint, "move", 25, point.y)]);
    assert.equal(dot(drag, 1).x, 25);
    assert.equal(dot(drag, 2).x, 65);
    assert.equal(drag.commands.length, 0);
    const moved = present([gesture(firstPoint, "finish", 25, point.y)]).commands[0];
    assert.equal(moved?.type, "edit");
    assert.equal(moved.event.payload["sample_index"], "120000");
    const other = present([acknowledge(moved, 2)]).commands[0];
    assert.equal(other?.type, "edit");
    assert.equal(other.gesture, moved.gesture);
    assert.equal(other.event.payload["sample_index"], "312000");
    const finished = present([acknowledge(other, 3)]).commands[0];
    assert.equal(finished?.type, "finish");
    present([acknowledge(finished, 3)]);
    present([gesture(firstPoint, "begin", 25, point.y)]);
    const lost = present([], { revision: 4, node: { ...snapshot.node, arguments: [[points[1]]] } });
    assert.equal(lost.cancelDrag, true);
    assert.ok(!lost.marks?.some(({ id }) => id === dotId(0)));
});

test("a tension handle edits the segment and keeps its stable hit target", (t) => {
    const present = harness(t);
    const target = present().targets?.find(({ id }) => id === firstTension);
    assert.ok(target?.cursor === "ew-resize");
    const x = target.x + target.width / 2, y = target.y + target.height / 2;
    present([gesture(firstTension, "begin", x, y)]);
    const draft = present([gesture(firstTension, "move", x - 25, y + 300)], { clockMs: 90 });
    const edit = draft.commands[0];
    assert.equal(edit?.type, "edit");
    assert.deepEqual(edit.event.payload, { id: 1, interpolation: "bezier", tension: .75 });
    assert.equal(draft.targets?.find(({ id }) => id === firstTension)?.hint, "Tension 0.75");
    const released = present([gesture(firstTension, "finish", x - 25, y + 300), acknowledge(edit, 2)], { clockMs: 100 });
    assert.equal(released.commands[0]?.type, "finish");
    assert.equal(released.drag?.cursor, undefined);
});

test("holding Shift preserves the point type and releasing Shift applies the tracked curve crossing", (t) => {
    const pulse = [
        { ...points[0], id: 1, seconds: 0, value: 1, interpolation: "hold" },
        { ...points[1], id: 2, seconds: 2.5 },
        { ...points[0], id: 3, seconds: 10, value: 0, tension: .5 },
        { ...points[1], id: 4, seconds: 12, value: 0 },
    ];
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, arguments: [pulse] } });
    const point = dot(present(), 2);
    const target = "5";
    present([gesture(target, "begin", point.x, point.y)]);
    present([
        gesture(target, "modifiers", point.x, point.y, { modifiers: { ...modifiers, shift: true } }),
        gesture(target, "move", 116, point.y),
        gesture(target, "move", 75, point.y),
        gesture(target, "modifiers", 75, point.y),
    ], { clockMs: 25 });
    const edit = present([{ type: "wake" }], { clockMs: 90 }).commands[0];
    assert.equal(edit?.type, "edit");
    assert.deepEqual(edit.event.payload, { insert: false, ordered_ids: [],
        point: { id: 2, seconds: 0, sample_index: "360000", value: 1, interpolation: "bezier", tension: .5, order: 0 },
        segments: [{ id: 1, interpolation: "empty", tension: 1 }] });
});

test("point edits require an accepted invocation, including a valid default constructor", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, constructionDefinitionId: undefined, arguments: [] } });
    const staleAdd = gesture(addTarget, "begin", 20, 50);
    const missing = present([staleAdd]);
    assert.equal(missing.itemEditing, undefined);
    assert.equal(missing.targets?.some(({ id }) => id === addTarget), false);
    assert.deepEqual(missing.commands, []);

    const oversized = Array.from({ length: 17000 }, (_, id) => ({ ...points[0], id, seconds: id }));
    const pending = present([staleAdd], { node: { ...snapshot.node, arguments: [oversized] } });
    assert.equal(pending.status, "Waiting for accepted Automation Curve arguments");
    assert.equal(pending.itemEditing, undefined);
    assert.deepEqual(pending.commands, []);

    const editable = present([staleAdd], { node: { ...snapshot.node, arguments: [] } });
    assert.equal(editable.itemEditing, true);
    const command = editable.commands[0];
    assert.equal(command?.type, "edit");
    assert.equal(command.event.payload["insert"], true);
});

test("retained automation geometry and hit targets reproject through the sample seam without a guest frame", (t) => {
    const seam = 1n << 63n;
    const node = { ...snapshot.node, arguments: [[
        { ...points[0], sample_index: String(seam - 8n), value: 0, tension: 1 },
        { ...points[1], sample_index: String(-seam + 8n), value: 1 },
    ]] };
    const initial = { ...snapshot, node, viewport: { ...snapshot.viewport,
        time: { beginSample: String(seam - 8n), beginFraction: 0, spanSamples: 16, sampleRate: 48000 } } };
    const present = createWasmPresentation(artifact.toString("base64"));
    t.after(() => present.dispose());
    present.restore("");
    const frame = readPresentationFrame(present.present(wasmPresentationUpdate(initial, [])).frame);
    assert.ok(frame.timelineCurve);
    const original = projectTimelineCurveFrame(frame, initial);
    const first = trace(original, tracks.authored).flat().find(({ x }) => x === 25);
    assert.ok(first);
    const expected = (1 - (.5 + Math.atan(.15625) / Math.PI)) * 100;
    assert.ok(Math.abs(first.y - expected) < 1e-9);
    const panned = projectTimelineCurveFrame(frame, { ...initial, viewport: { ...initial.viewport,
        time: { ...initial.viewport.time, beginSample: String(seam - 4n) } } });
    assert.ok(Math.abs(trace(panned, tracks.authored)[0][0].y - first.y) < 1e-9);
    assert.equal(dot(panned, 2).x, 75);
    assert.equal(panned.targets?.find(({ id }) => id === "5")?.x, 67);
});

test("a queued add-point press uses its captured viewport and scale after both have changed", (t) => {
    const seam = 1n << 63n;
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, arguments: [[]] } });
    const pickup = { width: 200, height: 80,
        time: { beginSample: String(seam - 7n), beginFraction: .25, spanSamples: 100, sampleRate: 48000 } };
    const shifted = { ...snapshot.viewport,
        time: { ...snapshot.viewport.time!, beginSample: "5000", spanSamples: 200 } };
    const frame = present([
        { type: "valueView", action: "pan", fraction: .25 },
        gesture(addTarget, "begin", 20, 40, { viewport: pickup, curveAxis: { pan: 0, zoom: 1, center: .5, cropped: false } }),
    ], { viewport: shifted });
    const command = frame.commands[0];
    assert.equal(command?.type, "edit");
    assert.deepEqual(command.event.payload["point"], {
        id: 0, seconds: 0, sample_index: String(-seam + 3n), value: 0, interpolation: "empty", tension: 1, order: 0,
    });
    assert.equal(parsePresentationState(frame.saveRequest!)?.pan, Math.PI / 4);
});

test("automation drags that request cursor return also request pointer lock", (t) => {
    const frame = harness(t)();
    const target = (id: string) => frame.targets?.find((item) => item.id === id);
    for (const id of [firstPoint, addTarget]) {
        assert.equal(target(id)?.cursorReturn, true);
        assert.equal(target(id)?.drag, "relative");
    }
    assert.equal(target(selectionTarget)?.drag, "absolute");
});

test("a missing connected bound leaves the authored curve and omits the output preview", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node,
        ports: [{ name: "max", direction: "input", kind: "continuous", connected: true, manualValue: 0 }] } });
    const frame = present();
    assert.ok(trace(frame, tracks.authored).length);
    assert.deepEqual(trace(frame, tracks.output), []);
    assert.deepEqual(trace(frame, tracks.max), []);
});

test("an observed empty bound interval permits the output preview from the other bound", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, ports: [
        { name: "min", direction: "input", kind: "continuous", manualValue: .8 },
        { name: "max", direction: "input", kind: "continuous", connected: true },
    ] } });
    const result: ObservationWindowResult = { id: "max", status: "ready", approximate: false, regions: [],
        window: { begin: "0", length: "480000", points: 1024 } };
    const frame = present([], { observations: { max: { operation: "waveform", beginSample: "0", status: "ready", result,
        spans: [{ begin: "0", end: "480000", windowOffset: "0", result }] } } });
    assert.ok(trace(frame, tracks.output).length);
    assert.deepEqual(trace(frame, tracks.max), []);
});

test("the output preview follows the authored curve through a gap in both bounds", (t) => {
    const present = harness(t, { ...snapshot, node: { ...snapshot.node, ports: [
        { name: "min", direction: "input", kind: "continuous", connected: true },
        { name: "max", direction: "input", kind: "continuous", connected: true },
    ] } });
    const observedBound = (id: string, begin: string, length: string, value: number): PresentationObservation => {
        const result: ObservationWindowResult = { id, status: "ready", approximate: false,
            window: { begin: "0", length: "480000", points: 1024 },
            regions: [{ begin, length, points: [{ index: begin, available: true, value: { value } }] }] };
        return { operation: "waveform", beginSample: "0", status: "ready", result,
            spans: [{ begin: "0", end: "480000", windowOffset: "0", result }] };
    };
    const min = observedBound("min", "48000", "48000", .8);
    const max = observedBound("max", "192000", "48000", .7);
    const frame = present([], { observations: { min, max } });
    const sampleAt = (frame: PresentationFrame, track: number, x: number) =>
        trace(frame, track).flat().find((point) => point.x === x);
    const middle = sampleAt(frame, tracks.authored, 30);
    assert.ok(middle);
    assert.deepEqual(sampleAt(frame, tracks.output, 30), middle);
    assert.ok(sampleAt(frame, tracks.output, 15)!.y < sampleAt(frame, tracks.authored, 15)!.y);
    assert.ok(sampleAt(frame, tracks.output, 45)!.y > sampleAt(frame, tracks.authored, 45)!.y);
    assert.equal(sampleAt(frame, tracks.output, 60), undefined);

    const pending = present([], { observations: { min, max: null } });
    assert.equal(sampleAt(pending, tracks.output, 30), undefined);
});

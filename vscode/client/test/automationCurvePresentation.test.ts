import assert from "node:assert/strict";
import { test } from "node:test";
import { createAutomationCurvePresentation } from "../src/automationCurvePresentation";
import type { ObservationWindowResult } from "../src/observations";
import { readPresentationFrame, type PresentationCommand, type PresentationFrame, type PresentationGestureInput,
    type PresentationInput, type PresentationSnapshot } from "../src/presentationPlugin";

const points = [
    { id: 1, seconds: 1, value: .5, interpolation: "bezier", tension: 1 },
    { id: 2, seconds: 5, value: 1, interpolation: "empty", tension: 1 },
];
const snapshot: PresentationSnapshot = {
    node: { definitionId: "builtin.automation.curve", address: { instanceId: "curve" }, arguments: [points], ports: [] },
    mode: "editable", editable: true, clockMs: 0, revision: 1, state: null, observations: {},
    viewport: { width: 100, height: 100, time: { beginSample: "0", beginFraction: 0, spanSamples: 480000, sampleRate: 48000 } },
};
const modifiers = { shift: false, alt: false, control: false, meta: false };
function gesture(id: string, phase: PresentationGestureInput["phase"], x: number, y: number,
    options?: Partial<PresentationGestureInput>): PresentationGestureInput {
    return { type: "gesture", id, phase, point: { x, y }, modifiers, ...options };
}
function harness(initial = snapshot) {
    const present = createAutomationCurvePresentation();
    return (inputs: PresentationInput[] = [], update?: Partial<PresentationSnapshot>) =>
        readPresentationFrame(present({ ...initial, ...update }, inputs));
}
function dot(frame: PresentationFrame, id: number) {
    const mark = frame.marks?.find((item) => item.id === `dot:${id}`);
    assert.equal(mark?.type, "circle");
    return mark;
}
function acknowledge(command: PresentationCommand | undefined, acceptedRevision: number): PresentationInput {
    assert.ok(command && "requestId" in command && command.requestId);
    return { type: "editResult", requestId: command.requestId, acceptedRevision };
}
const settle = () => new Promise<void>((resolve) => setImmediate(resolve));

test("curve gestures keep exact playhead detents, send drafts before release and close after acknowledgement", async () => {
    const present = harness();
    const point = dot(present(), 1);
    present([gesture("point:1", "begin", point.x, point.y, { snapping: { marks: [], playhead: "96000" } })]);
    const moving = present([gesture("point:1", "move", 20, point.y)], { clockMs: 10 });
    assert.equal(dot(moving, 1).x, 20);
    assert.ok(moving.marks?.some(({ id }) => id === "time-snap"));
    assert.deepEqual(moving.commands, []);
    assert.equal(moving.wakeAfterMs, 80);
    const paced = present([{ type: "wake" }], { clockMs: 90 });
    const move = paced.commands?.[0];
    assert.equal(move?.type, "edit");
    assert.deepEqual(move.event.payload, { id: 1, seconds: 0, sample_index: "96000", value: .5 });

    const release = present([gesture("point:1", "finish", 20, point.y)], { clockMs: 95 });
    assert.deepEqual(release.drag?.cursor, { x: 20, y: point.y });
    assert.deepEqual(release.commands, []);
    present([acknowledge(move, 2)]);
    await settle();
    const finish = present([{ type: "wake" }]).commands?.[0];
    assert.equal(finish?.type, "finish");
    assert.equal(finish.gesture, move.gesture);
    present([acknowledge(finish, 2)]);
    await settle();
    const held = present();
    assert.equal(dot(held, 1).x, 20);
    assert.equal(held.drag, undefined);
    assert.equal(held.marks?.some(({ id }) => id === "ring:1"), false);
    const published = present([], { revision: 2, node: { ...snapshot.node,
        arguments: [[{ ...points[0], seconds: 2, sample_index: "96000" }, points[1]]] } });
    assert.equal(dot(published, 1).x, 20);
});

test("the curve flushes a new point immediately and history discards an unsent move", async () => {
    const present = harness({ ...snapshot, node: { ...snapshot.node, arguments: [[]] } });
    const added = present([gesture("add", "begin", 20, 50)]);
    const insert = added.commands?.[0];
    assert.equal(insert?.type, "edit");
    assert.deepEqual(insert.event.payload, { insert: true, ordered_ids: [], segments: [],
        point: { id: 0, seconds: 0, sample_index: "96000", value: 0, interpolation: "empty", tension: 1, order: 0 } });
    present([gesture("add", "move", 30, 40)], { clockMs: 10 });
    const interrupted = present([{ type: "interrupt", reason: "history" }], { clockMs: 100 });
    assert.equal(interrupted.cancelDrag, true);
    assert.deepEqual(interrupted.commands, []);
    present([acknowledge(insert, 2)]);
    await settle();
    const closing = present([{ type: "wake" }], { clockMs: 101 });
    assert.deepEqual(closing.commands?.map(({ type }) => type), ["finish"]);
});

test("rectangle selection and contextual controls use the same frame as the curve", () => {
    const present = harness();
    const first = present();
    assert.equal(first.targets?.find(({ id }) => id === "selection")?.drag, "absolute");
    assert.equal(first.targets?.find(({ id }) => id === "add")?.button, "secondary");
    const selection = present([gesture("selection", "begin", 0, 13), gesture("selection", "finish", 70, 80)]);
    assert.ok(selection.marks?.some(({ id }) => id === "ring:1"));
    assert.ok(selection.marks?.some(({ id }) => id === "ring:2"));
    const toggle = { modifiers: { ...modifiers, control: true } };
    const second = present([gesture("selection", "begin", 3, 16, toggle), gesture("selection", "finish", 19, 80, toggle),
        { type: "context", id: "tension:1", point: { x: 10, y: 45 } }]);
    assert.equal(second.marks?.some(({ id }) => id === "ring:1"), false);
    assert.ok(second.marks?.some(({ id }) => id === "ring:2"));
    const knob = second.contextMenu?.rows.flat().find(({ id }) => id === "tension");
    assert.equal(knob?.type, "knob");
    assert.equal(knob.value, 1);
    const command = present([{ type: "items", action: "delete" }]).commands?.[0];
    assert.equal(command?.type, "edit");
    assert.deepEqual(command.event.payload, { id: 2 });
});

test("scale commands save the curve view and a reloaded lane draws it over a manual bound", () => {
    const present = harness();
    const scale = present([{ type: "control", id: "display", phase: "commit", value: "cropped" },
        { type: "valueView", action: "zoom", factor: 2, y: .5 }]);
    const command = scale.commands?.[0];
    assert.equal(command?.type, "state");
    assert.equal(command.value?.display, "cropped");
    const restored = harness({ ...snapshot, state: command.value, node: { ...snapshot.node,
        ports: [{ name: "min", kind: "continuous", direction: "input", manualValue: .8 }] } });
    const frame = restored();
    assert.ok(frame.marks?.some(({ id }) => id === "min:0"));
    assert.ok(frame.marks?.some(({ id }) => id === "output:0"));
    assert.deepEqual(restored([{ type: "valueView", action: "reset" }]).commands,
        [{ type: "state", value: { display: "cropped" } }]);
});

test("a connected bound uses the retained spans while its replacement is pending", () => {
    const present = harness({ ...snapshot, node: { ...snapshot.node,
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
        const mark = frame.marks?.find(({ id }) => id === "max:0");
        assert.equal(mark?.type, "polyline");
        return mark.points;
    };
    const first = maxTrace(present([], { observations: { max: { status: "ready", result, spans } } }));
    assert.equal(first[0].y, 50);
    assert.equal(first.at(-1)?.y, 25);
    const pending = { ...result, status: "pending" as const, regions: [] };
    assert.deepEqual(maxTrace(present([], { observations: { max: { status: "pending", result: pending, spans } } })), first);
});

test("automation points and interpolation follow the same view through the signed seam", () => {
    const seam = 1n << 63n;
    const pointsAcross = [
        { ...points[0], id: 21, sample_index: String(seam - 2n), value: 0, interpolation: "bezier", tension: 0 },
        { ...points[0], id: 22, sample_index: String(-seam + 2n), value: 1, interpolation: "empty" },
    ];
    const time = { beginSample: String(seam - 5n), beginFraction: 0, spanSamples: 10, sampleRate: 48000 };
    const present = harness({ ...snapshot, viewport: { ...snapshot.viewport, time },
        node: { ...snapshot.node, arguments: [pointsAcross] } });
    const frame = present();
    const first = frame.targets?.find(({ id }) => id === "point:21");
    const second = frame.targets?.find(({ id }) => id === "point:22");
    assert.ok(first && second);
    assert.ok(first.x < 50 && second.x > 50);
    const traces = frame.marks?.flatMap((mark) => mark.type === "polyline" && mark.id.startsWith("authored:") ? [mark.points] : []) ?? [];
    assert.ok(Math.abs(traces[0][0].x - 30) < 1e-9);
    assert.ok(Math.abs(traces.at(-1)!.at(-1)!.x - 70) < 1e-9);
    const midpoint = traces.flat().filter(({ x }) => Math.abs(x - 50) < 1e-9);
    assert.ok(midpoint.length && midpoint.every(({ y }) => Math.abs(y - midpoint[0].y) < 1e-9));
});

test("a densely authored curve fits the bounded frame and retains points across the viewport", () => {
    const dense = points[0];
    const present = harness({ ...snapshot, viewport: { ...snapshot.viewport, width: 4000 },
        node: { ...snapshot.node, arguments: [Array.from({ length: 7000 }, (_, i) => ({
            ...dense, id: i, seconds: 10 * i / 7000, value: i % 2, interpolation: i % 3 === 0 ? "empty" : "bezier",
        }))], ports: [{ name: "min", direction: "input", kind: "continuous", manualValue: .1 },
            { name: "max", direction: "input", kind: "continuous", manualValue: .5 }] },
    });
    const frame = present();
    assert.ok(frame.targets?.some(({ id, x }) => id.startsWith("point:") && x > 3900));
    assert.ok(frame.targets?.some(({ id, x }) => id.startsWith("point:") && x < 100));
});

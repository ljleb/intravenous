import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { openPresentationTimelineReader, renderPresentationTimelineGrid } from "../src/presentationTimelineGrid";
import { laneViewFromKey, presentationStateKey, readGraphViewState } from "../src/graphScope";
import { SetBeatGrid } from "../src/generated/beatsEvents";
import { PlacePoint } from "../src/generated/automationEvents";
import { serializeNodeEvent } from "../src/nodeEvents";
import { PresentationEdits } from "../src/presentationEdits";
import { PresentationLaneEdits } from "../src/presentationLaneEdits";
import { presentationPluginVersion, readPresentationFrame, readPresentationManifest,
    type PresentationCommand, type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "../src/presentationPlugin";
import { PresentationPluginSession, PresentationWake } from "../src/presentationPluginSession";
import { maxPresentationStateBytes, serializePresentationState } from "../src/presentationStateJson";
import { PresentationArtifactReader } from "../src/presentationArtifacts";
import { createWasmPresentation, readWasmPresentationDescriptor, wasmPresentationUpdate } from "../src/wasmPresentation";
import { createNodePresentationWorker } from "../src/nodeWasmPresentationWorker";
import { WasmPresentationWorker } from "../src/wasmPresentationWorker";
import { ReloadableWasmPresentation } from "../src/reloadableWasmPresentation";
import { packWasmArguments } from "../src/wasmPresentationArguments";
import { compileLayout } from "../src/wasmPresentationValues";

const snapshot: PresentationSnapshot = {
    node: { definitionId: "builtin.beats", arguments: [120, 4, [2]], ports: [] },
    revision: 1, mode: "editable", editable: true, viewport: { width: 500, height: 80,
        time: { beginSample: "0", beginFraction: 0, spanSamples: 96000, sampleRate: 48000 } }, observations: {}, state: null,
};

const wasmFixture = (name: string) => readFileSync(resolve(__dirname, "../presentations", `${name}.wasm`));
const clickDetector = wasmFixture("click_detector");
const clickManifest = JSON.parse(readFileSync(resolve(__dirname,
    "../../../../src/intravenous/builtin_packages/builtin/iv_package.json"), "utf8")).presentations.click_detector;

test("the published Click Detector WASM draws retained trigger columns through the headless worker", async () => {
    assert.ok(readPresentationManifest({ ...clickManifest, version: presentationPluginVersion, id: "click_detector" }));
    assert.deepEqual(readWasmPresentationDescriptor(clickDetector), { timeline: true, icon: "M1 8h4l2-5 2 10 2-5h4" });
    const worker = createNodePresentationWorker(clickDetector.toString("base64"));
    const result = {
        id: "clicks", status: "ready" as const, approximate: true,
        window: { begin: "0", length: "96000", points: 500 },
        triggerBins: [
            { begin: "24000", length: "192", first: "24000", count: "1" },
            { begin: "48000", length: "192", first: "48000", count: "3" },
        ], regions: [],
    };
    try {
        const frame = readPresentationFrame(await worker.run({
            ...snapshot, observations: { clicks: { operation: "trigger-bins" as const, status: "pending", result,
                spans: [{ begin: "0", end: "96000", windowOffset: "0", result }] } },
        }, []));
        assert.equal(frame.status, "4 recorded clicks in view");
        assert.deepEqual(frame.marks?.filter((mark) => mark.type === "packedPolyline")
            .map(({ coordinates }) => Array.from(coordinates)), [[125.5, 0, 125.5, 80], [250.5, 0, 250.5, 80]]);
        assert.equal(frame.marks?.at(-1)?.type, "text");
    } finally { worker.stop(); }
});

test("Click Detector bounds drawing while preserving large counts and hides its label in mini lanes", () => {
    const instance = createWasmPresentation(clickDetector.toString("base64"));
    instance.restore("");
    try {
        const triggerBins = Array.from({ length: 500 }, (_, i) =>
            ({ begin: String(i * 192), length: "192", first: String(i * 192 + 1), count: "2000" }));
        const result = { id: "clicks", status: "ready" as const, approximate: true,
            window: { begin: "0", length: "96000", points: 500 }, triggerBins, regions: [] };
        const displayed = { ...snapshot, mode: "mini" as const,
            observations: { clicks: { operation: "trigger-bins" as const, status: "ready" as const, result,
                spans: [{ begin: "0", end: "96000", windowOffset: "0", result }] } },
        };
        const frame = readPresentationFrame(instance.present(wasmPresentationUpdate(displayed, [])).frame);
        assert.equal(frame.status, "1000000 recorded clicks in view");
        assert.equal(frame.marks?.length, 500);
        assert.ok(frame.marks?.every(({ type }) => type === "packedPolyline"));

        const clipped = { ...displayed, observations: { clicks: { operation: "trigger-bins" as const, status: "pending" as const, result,
            spans: [{ begin: "1", end: "192", windowOffset: "0", result }] } } };
        assert.equal(instance.present(wasmPresentationUpdate(clipped, [])).frame?.status, "At least 1 recorded click in view");

        const waiting = { ...snapshot, node: { ...snapshot.node,
            ports: [{ direction: "input" as const, kind: "continuous" as const, connected: true, name: "audio" }] } };
        assert.equal(instance.present(wasmPresentationUpdate(waiting, [])).frame?.status, "Waiting for click observations");
        assert.equal(instance.present(wasmPresentationUpdate(snapshot, [])).frame?.status, "Connect the audio to inspect");
    } finally { instance.dispose(); }
});

const beats = wasmFixture("beats").toString("base64");

test("the Beats artifact supplies settings and an exact grid that the host can seek without a worker", async () => {
    const worker = createNodePresentationWorker(beats);
    const view = snapshot.viewport.time!;
    const run = async (state: PresentationSnapshot, inputs: PresentationInput[] = []) =>
        readPresentationFrame(await worker.run(state, inputs));
    try {
        const first = await run(snapshot);
        assert.equal(first.panel?.label, "120 BPM");
        assert.ok(first.timelineGrid?.seek);
        const display = renderPresentationTimelineGrid(first.timelineGrid, view, 500, 80, true);
        assert.ok(display.samples.includes(24000n));
        assert.equal(display.marks.find(({ id }) => id === "grid:24000")?.type, "polyline");

        const second = await run(snapshot, [
            { type: "control", id: "bpm", phase: "preview", value: 140 },
            { type: "control", id: "bpm", phase: "preview", value: 150 },
            { type: "control", id: "bpm", phase: "finish" },
        ]);
        assert.equal(second.panel?.label, "150 BPM");
        const edit = second.commands?.[0];
        assert.equal(second.commands?.length, 2);
        assert.equal(edit?.type, "edit");
        assert.match(edit.gesture ?? "", /^wasm:\d+:\d+$/);
        assert.equal(edit.replacePending, true);
        assert.deepEqual(edit.event, serializeNodeEvent(SetBeatGrid({ bpm: 150, beats_per_bar: 4, subdivisions: [2] })));
        const finish = second.commands[1];
        assert.equal(finish.type, "finish");
        assert.equal(finish.gesture, edit.gesture);
        assert.match(finish.requestId ?? "", /^wasm:\d+:\d+$/);
        assert.equal((await run({ ...snapshot, editsPending: true })).panel?.label, "150 BPM");
        assert.ok(second.timelineGrid);
        const read = openPresentationTimelineReader(second.timelineGrid);
        const remote = { first: 96000n, length: 96000n };
        assert.equal((await run(snapshot)).panel?.label, "120 BPM");
        assert.ok(read.marks(remote, 48000, 500).includes(115200n));
        assert.equal((await run({ ...snapshot, editable: false }, [
            { type: "control", id: "bpm", phase: "commit", value: 150 },
        ])).commands, undefined);

        const edited = await run(snapshot, [
            { type: "control", id: "add", phase: "commit" },
            { type: "control", id: "subdivision:1", phase: "commit", value: 3 },
            { type: "control", id: "remove:0", phase: "commit" },
        ]);
        assert.equal(edited.commands?.length, 3);
        const commit = edited.commands?.at(-1);
        assert.equal(commit?.type, "edit");
        assert.equal(commit.gesture, undefined);
        assert.match(commit.requestId ?? "", /^wasm:\d+:\d+$/);
        assert.deepEqual(commit.event, serializeNodeEvent(SetBeatGrid({ bpm: 120, beats_per_bar: 4, subdivisions: [3] })));
        assert.equal(edited.panel?.rows.length, 4);
    } finally { worker.stop(); }
});

test("full-state Beats previews reach the backend during a drag and flush the latest value at release", async () => {
    const instance = createWasmPresentation(beats);
    instance.restore("");
    const preview = (bpm: number) => {
        const command = instance.present(wasmPresentationUpdate(snapshot, [
            { type: "control", id: "bpm", phase: "preview", value: bpm },
        ])).frame?.commands?.[0];
        assert.equal(command?.type, "edit");
        return command;
    };
    const sent: PresentationCommand[] = [];
    let release!: () => void;
    const first = new Promise<void>((resolve) => { release = resolve; });
    let alarm: (() => void) | undefined;
    const edits = new PresentationEdits(async (command) => {
        sent.push(command);
        if (sent.length === 1) await first;
    }, () => {}, (error) => assert.fail(String(error)), {
        set: (callback) => { alarm = callback; return 1; },
        clear: () => { alarm = undefined; },
    });
    const turn = () => new Promise<void>((resolve) => { setImmediate(resolve); });
    const fire = () => {
        const callback = alarm;
        assert.ok(callback);
        alarm = undefined;
        callback();
    };
    const a = preview(140), b = preview(160), c = preview(180);
    edits.enqueue(preview(130));
    edits.enqueue(a);
    assert.deepEqual(sent, []);
    fire();
    assert.deepEqual(sent, [a]);

    edits.enqueue(preview(150));
    edits.enqueue(b);
    release();
    await turn();
    assert.deepEqual(sent, [a]);
    fire();
    await turn();
    assert.deepEqual(sent, [a, b]);

    assert.ok(a.gesture);
    const finish = { type: "finish", gesture: a.gesture } as const;
    edits.enqueue(preview(170));
    edits.enqueue(c);
    edits.enqueue(finish);
    await turn();
    assert.equal(alarm, undefined);
    assert.equal(edits.pending, false);
    assert.deepEqual(sent, [a, b, c, finish]);
    instance.dispose();
});

test("ordered edits separate replaceable previews from each other", async () => {
    const sent: PresentationCommand[] = [];
    const edits = new PresentationEdits(async (command) => { sent.push(command); }, () => {},
        (error) => assert.fail(String(error)), { set: () => 1, clear: () => {} });
    const eventType = { nominalId: "test.edit", definitionFingerprint: "v1" };
    const edit = (payload: number, replacePending?: true): PresentationCommand =>
        ({ type: "edit", gesture: "test", replacePending, event: { eventType, payload } });
    const commands = [edit(1, true), edit(2), edit(3, true), { type: "finish", gesture: "test" } as const];
    for (const command of commands) edits.enqueue(command);
    await new Promise<void>((resolve) => { setImmediate(resolve); });
    assert.deepEqual(sent, commands);
});

test("a WASM placement previews while pending and draws from the accepted constructor arguments", async () => {
    const worker = createNodePresentationWorker(wasmFixture("placement").toString("base64"));
    const definitionId = "builtin.automation.curve";
    const initialPoint = { id: 21, seconds: 0, value: .25, interpolation: "hold", tension: 1,
        sample_index: "9223372036854775805", order: 0 };
    const argumentsBeforeEdit: unknown[] = [[initialPoint]];
    const curve = { ...snapshot, node: { ...snapshot.node, definitionId, constructionDefinitionId: definitionId,
        arguments: argumentsBeforeEdit } };
    const replies: PresentationInput[] = [];
    const requests: Array<ReturnType<typeof serializeNodeEvent>> = [];
    let accept!: () => void;
    let revision = curve.revision;
    let groups = 0;
    let finished = 0;
    const edits = new PresentationLaneEdits({
        editable: () => true,
        gesture: () => {
            ++groups;
            return {
                send: (event) => new Promise<void>((resolve) => {
                    requests.push(serializeNodeEvent(event));
                    accept = () => { ++revision; resolve(); };
                }),
                finish: async () => { ++finished; },
                acceptedRevision: () => revision,
            };
        },
        accepted: () => {}, changed: () => {}, setError: () => {},
        fail: (error) => assert.fail(String(error)),
        input: (input) => replies.push(input),
    });
    const run = async (...inputs: PresentationInput[]) => readPresentationFrame(await worker.run(curve, inputs));
    const drag = (phase: "begin" | "move" | "finish", y: number): PresentationInput => ({
        type: "gesture", id: "1", phase, point: { x: 250, y },
        modifiers: { shift: false, alt: false, control: false, meta: false },
    });
    const turn = () => new Promise<void>((resolve) => { setImmediate(resolve); });
    try {
        const before = (await run()).marks?.[0];
        assert.equal(before?.type, "packedPolyline");
        assert.deepEqual(Array.from(before.coordinates), [0, 60, 500, 60]);
        const initial = await run(drag("begin", 40));
        const insert = initial.commands?.[0];
        assert.equal(insert?.type, "edit");
        assert.ok(insert.requestId);
        edits.accept(initial.commands);
        assert.deepEqual(requests, [serializeNodeEvent(PlacePoint({
            point: { id: 17, seconds: 0, value: .5, interpolation: "hold", tension: 1,
                sample_index: "9223372036854775805", order: 1 },
            insert: true, segments: [{ id: 12, interpolation: "empty", tension: 1 }], ordered_ids: [21, 17],
        }))]);

        const pending = await run(drag("move", 20), drag("finish", 20), {
            type: "editResult", requestId: `wasm:0:${insert.requestId.split(":").at(-1)}`, acceptedRevision: 2,
        });
        assert.equal(pending.commands, undefined);
        assert.equal(pending.status, "Placement pending");
        const mark = pending.marks?.[0];
        assert.equal(mark?.type, "packedPolyline");
        assert.deepEqual(Array.from(mark.coordinates), [0, 20, 500, 20]);
        accept();
        await turn();
        assert.deepEqual(replies, [{ type: "editResult", requestId: insert.requestId, acceptedRevision: 2 }]);

        const moved = await run(...replies.splice(0));
        const move = moved.commands?.[0];
        assert.equal(move?.type, "edit");
        assert.equal(move.gesture, insert.gesture);
        assert.notEqual(move.requestId, insert.requestId);
        assert.deepEqual(move.event, serializeNodeEvent(PlacePoint({
            point: { id: 17, seconds: 0, value: .75, interpolation: "hold", tension: 1,
                sample_index: "9223372036854775805", order: 1 },
            insert: false, segments: [{ id: 12, interpolation: "empty", tension: 1 }], ordered_ids: [21, 17],
        })));
        edits.accept(moved.commands);
        accept();
        await turn();
        const end = await run(...replies.splice(0));
        assert.equal(end.commands?.[0]?.type, "finish");
        edits.accept(end.commands);
        await turn();
        assert.equal((await run(...replies.splice(0))).status, "Placement pending");
        assert.equal(readPresentationFrame(await worker.run({ ...curve, revision }, [])).status,
            "Placement missing from accepted arguments");

        const placed = { id: 17, seconds: 0, value: .625, interpolation: "hold", tension: 1,
            sample_index: "9223372036854775805", order: 1 };
        const readBack = { ...curve, revision, node: { ...curve.node, arguments: [[initialPoint, placed]] } };
        for (let i = 0; i < 2; ++i) {
            const frame = readPresentationFrame(await worker.run(readBack, []));
            assert.equal(frame.status, "Placement accepted");
            const acceptedMark = frame.marks?.[0];
            assert.equal(acceptedMark?.type, "packedPolyline");
            assert.deepEqual(Array.from(acceptedMark.coordinates), [0, 30, 500, 30]);
        }
        const missing = { ...readBack, node: { ...readBack.node, constructionDefinitionId: undefined } };
        assert.equal(readPresentationFrame(await worker.run(missing, [])).status, "Placement missing from accepted arguments");
        assert.equal(groups, 1);
        assert.equal(finished, 1);
    } finally {
        edits.queue.dispose();
        await edits.closeGestures();
        worker.stop();
    }
});

test("a package waveform borrows retained regions with exact native indices across the signed seam", async () => {
    const program = wasmFixture("placement").toString("base64");
    const begin = "9223372036854775806";
    const node = { ...snapshot.node, definitionId: "builtin.automation.curve" };
    const result = { id: "value", status: "ready" as const, approximate: true,
        window: { begin, length: "4", points: 4 }, regions: [{ begin, length: "4", points: [
            { index: begin, available: true, value: { value: .25 } },
            { index: "9223372036854775807", available: false },
            { index: "-9223372036854775808", available: true, value: { value: .75 } },
            { index: "-9223372036854775807", available: true, value: { value: .5 } },
        ] }] };
    const display: PresentationSnapshot = { ...snapshot, node,
        viewport: { ...snapshot.viewport, time: { beginSample: begin, beginFraction: 0, spanSamples: 4, sampleRate: 48000 } },
        observations: { value: { operation: "waveform", status: "pending",
            spans: [{ begin: "0", end: "4", windowOffset: "0", result }] } },
    };
    const update = wasmPresentationUpdate(display, []);
    assert.equal(update.triggerColumns.length, 0);
    assert.equal(update.waveforms[0].status, "pending");
    assert.deepEqual(update.waveforms[0].regions.map(({ begin, end }) => [begin, end]), [[0, 1], [2, 4]]);
    const points = new DataView(update.waveforms[0].regions[1].points);
    assert.equal(points.getBigInt64(0, true), -9223372036854775808n);
    const worker = createNodePresentationWorker(program);
    try {
        const frame = readPresentationFrame(await worker.run(display, []));
        const lines = frame.marks?.flatMap((mark) => mark.type === "packedPolyline" && mark.ink === "output"
            ? [Array.from(mark.coordinates)] : []) ?? [];
        assert.deepEqual(lines, [[0, 60], [250, 20, 375, 40]]);
    } finally { worker.stop(); }
});

test("the host projects Beats immediately at a new zoom without another guest frame", () => {
    const instance = createWasmPresentation(beats);
    instance.restore("");
    try {
        const frame = readPresentationFrame(instance.present(wasmPresentationUpdate(snapshot, [])).frame);
        const grid = frame.timelineGrid!;
        const view = snapshot.viewport.time!;
        const beat = (marks: ReturnType<typeof renderPresentationTimelineGrid>["marks"]) =>
            marks.find(({ id }) => id === "grid:24000");
        const first = beat(renderPresentationTimelineGrid(grid, view, 500, 80, true).marks);
        assert.equal(first?.type, "polyline");
        assert.deepEqual(first.points.at(-1), { x: 125, y: 80 });
        const zoomed = beat(renderPresentationTimelineGrid(grid, { ...view, spanSamples: 48000 }, 500, 160, true).marks);
        assert.equal(zoomed?.type, "polyline");
        assert.deepEqual(zoomed.points.at(-1), { x: 250, y: 160 });
    } finally { instance.dispose(); }
});

test("a slow presentation preserves gesture ordering while coalescing viewport changes", async () => {
    const calls: Array<{ width: number; inputs: readonly PresentationInput[] }> = [];
    const replies: Array<(frame: PresentationFrame) => void> = [];
    const accepted: Array<{ draw: boolean; hasInput: boolean }> = [];
    const saves: string[] = [];
    const errors: unknown[] = [];
    const session = new PresentationPluginSession((view, inputs) => {
        calls.push({ width: view.viewport.width, inputs });
        return new Promise((resolve) => replies.push(resolve));
    }, () => {}, (_frame, draw, inputs) => accepted.push({ draw, hasInput: inputs.length > 0 }), (error) => errors.push(error), {
        canSave: () => true, save: (state) => { assert.ok(state); saves.push(state); },
        restore: () => assert.fail("The save should be kept when the viewport changes"),
    });
    const modifiers = { shift: false, control: false, alt: false, meta: false };
    const begin: PresentationInput = { type: "gesture", id: "handle", phase: "begin", point: { x: 5, y: 12 }, modifiers };
    const move: PresentationInput = { ...begin, phase: "move", point: { x: 8, y: 12 }, delta: { x: 3, y: 0 } };
    const changed: PresentationInput = { ...begin, phase: "modifiers", point: move.point, modifiers: { ...modifiers, shift: true } };
    const finish: PresentationInput = { ...changed, phase: "finish" };

    session.update(snapshot);
    session.input(begin);
    const first = session.flush();
    session.update({ ...snapshot, viewport: { ...snapshot.viewport, width: 600 } });
    session.input(move);
    session.update({ ...snapshot, viewport: { ...snapshot.viewport, width: 700 } });
    session.input(changed);
    session.input(finish);
    await session.flush();
    const saveRequest = '{"x":5}';
    replies[0]({ saveRequest });
    await first;
    assert.deepEqual(accepted, [{ draw: false, hasInput: true }]);
    assert.deepEqual(saves, [saveRequest]);

    const second = session.flush();
    assert.deepEqual(calls, [{ width: 500, inputs: [begin] }, { width: 700, inputs: [move, changed, finish] }]);
    session.dispose();
    replies[1]({});
    await second;
    assert.equal(accepted.length, 1);
    assert.deepEqual(errors, []);
});

test("invalid frames fail without publishing commands or visuals", async () => {
    assert.throws(() => readPresentationFrame({ timelineGrid: {
        numerator: "48000", denominator: "0", strides: ["1"], lineStarts: [0],
        opacity: .84, minimumSpacing: 8, seek: true, labelMajor: true,
    } }), /Invalid presentation frame/);
    assert.throws(() => readPresentationFrame({ marks: [
        { type: "rect", id: "spot", x: 0, y: 0, width: 1, height: 1 },
        { type: "text", id: "spot", text: "spot", x: 0, y: 0 },
    ] }), /Duplicate presentation item ID/);
    const errors: unknown[] = [];
    const session = new PresentationPluginSession(async () => ({ saveRequest: '{"x":1e999}' }),
        () => {}, () => assert.fail("Published an invalid frame"), (error) => errors.push(error));
    session.update(snapshot);
    await session.flush();
    assert.match(String(errors[0]), /Invalid presentation save number/);
});

test("history discards queued controls and the commands from an in-flight worker frame", async () => {
    const control = { type: "control", id: "bpm", phase: "preview", value: 144 } as const;
    const inputs: Array<readonly PresentationInput[]> = [];
    const accepted: Array<readonly PresentationInput[]> = [];
    const replies: Array<(value: PresentationFrame) => void> = [];
    const session = new PresentationPluginSession((_, batch) => {
        inputs.push(batch);
        return new Promise<PresentationFrame>((resolve) => replies.push(resolve));
    }, () => {}, (_, __, batch) => accepted.push(batch), (error) => assert.fail(String(error)));
    session.update(snapshot);
    session.input(control);
    const inFlight = session.flush();
    session.input(control);
    session.interrupt("history");
    replies[0]({ commands: [{ type: "finish", gesture: "bpm", requestId: "lost-finish" }] });
    await inFlight;
    const interruption = session.flush();
    replies[1]({});
    await interruption;
    const expected = [{ type: "interrupt", reason: "history" },
        { type: "editResult", requestId: "lost-finish", error: "Presentation edit interrupted" }];
    assert.deepEqual(inputs, [[control], expected]);
    assert.deepEqual(accepted, [expected]);
    session.dispose();
});

const marker = wasmFixture("marker");
const moduleBytes = marker.toString("base64");
// A custom section makes this a distinct artifact with the same behavior.
const replacementMarker = Buffer.concat([marker, Buffer.from([0, 2, 1, 120])]).toString("base64");

test("a worker frame can save state and submit the full batch of 32 edits", async () => {
    const event = serializeNodeEvent(SetBeatGrid({ bpm: 120, beats_per_bar: 4, subdivisions: [2] }));
    const commands: PresentationCommand[] = Array.from({ length: 32 }, (_, index) =>
        ({ type: "edit", event, requestId: String(index + 1) }));
    const saveRequest = '{"position":0.25}';
    const worker = new WasmPresentationWorker("", (receive) => ({
        postMessage: (message) => queueMicrotask(() => receive("close" in message ? { closed: true } : { frame: { commands, saveRequest } })),
        terminate: () => {},
    }));
    try {
        const frame = readPresentationFrame(await worker.run(snapshot, []));
        assert.deepEqual(frame.commands, commands);
        assert.equal(frame.saveRequest, saveRequest);
        assert.throws(() => readPresentationFrame({ ...frame, commands: [...commands, commands[0]] }), /Invalid presentation frame/);
        assert.equal(readPresentationFrame({ commands, saveRequest: "null" }).saveRequest, null);
    } finally { worker.stop(); }
});

test("presentation saves use the ABI's UTF-8 byte limit", () => {
    const serialize = (value: string) => JSON.stringify({ value });
    const overhead = serialize("").length;
    const exact = serialize("a".repeat(maxPresentationStateBytes - overhead));
    assert.equal(readPresentationFrame({ saveRequest: exact }).saveRequest, exact);
    const multibyte = serialize("é".repeat(36000));
    assert.equal(readPresentationFrame({ saveRequest: multibyte }).saveRequest, multibyte);
    const oversized = serialize("é".repeat(Math.floor((maxPresentationStateBytes - overhead) / 2) + 1));
    assert.throws(() => readPresentationFrame({ saveRequest: oversized }), /Invalid presentation save payload/);
    assert.throws(() => readPresentationFrame({ saveRequest: "[]" }), /Invalid presentation save payload/);
});

test("a discarded Marker save is rolled back in the live worker and on restart or replacement", async () => {
    const presentation = new ReloadableWasmPresentation(createNodePresentationWorker, (error) => { if (error) throw error; });
    let view = snapshot;
    let bytes = moduleBytes;
    let canSave = true;
    const frames: PresentationFrame[] = [];
    const proposedSaves: Array<PresentationFrame["saveRequest"]> = [];
    const session = new PresentationPluginSession(async (view, inputs) => {
        const frame = await presentation.run(bytes, view, inputs, () => true);
        if (frame?.saveRequest !== undefined) proposedSaves.push(frame.saveRequest);
        return frame;
    }, () => {}, (frame, draw) => { if (draw) frames.push(frame); }, (error) => { throw error; }, {
        canSave: () => canSave,
        save: (state) => {
            view = { ...view, state };
            session.update(view);
        },
        restore: (state) => presentation.restore(state),
    });
    const finishAt = (x: number): PresentationInput => ({ type: "gesture", id: "1", phase: "finish", point: { x, y: 40 },
        modifiers: { shift: false, alt: false, control: false, meta: false } });
    const markerX = () => {
        const line = frames.at(-1)?.marks?.[0];
        assert.equal(line?.type, "packedPolyline");
        return line.coordinates[0];
    };
    try {
        session.update(view);
        session.input(finishAt(125));
        await session.flush();
        assert.equal(view.state, '{"position":0.25}');

        session.input(finishAt(375));
        const pending = session.flush();
        session.interrupt("history");
        await pending;
        assert.deepEqual(proposedSaves, ['{"position":0.25}', '{"position":0.75}']);
        assert.equal(frames.length, 1);
        assert.equal(view.state, '{"position":0.25}');

        await session.flush();
        assert.equal(markerX(), 125);

        canSave = false;
        session.input(finishAt(450));
        await session.flush();
        assert.equal(markerX(), 125);
        session.update(view);
        await session.flush();
        assert.equal(markerX(), 125);

        presentation.suspend();
        session.update(view);
        await session.flush();
        assert.equal(markerX(), 125);

        bytes = replacementMarker;
        session.update(view);
        await session.flush();
        assert.equal(markerX(), 125);
    } finally { session.dispose(); presentation.dispose(); }
});

test("a remounted WASM lane restores the original compact JSON and its negative zero", async () => {
    const address = { instanceId: "marker" };
    const key = presentationStateKey(address, "marker");
    const state = `{"position":-0,"values":[${"1e10,".repeat(19999)}1e10]}`;
    assert.ok(Buffer.byteLength(state) <= maxPresentationStateBytes);
    assert.ok(Buffer.byteLength(JSON.stringify(JSON.parse(state))) > maxPresentationStateBytes);
    const laneView = laneViewFromKey(key, state);
    const reopened = JSON.parse(JSON.stringify({ laneOrder: [], laneViews: [laneView], timelineSnapMemory: {} }));
    const restored = readGraphViewState(reopened).presentationStates[key];
    assert.equal(restored, state);
    const worker = createNodePresentationWorker(moduleBytes);
    try {
        const frame = readPresentationFrame(await worker.run({ ...snapshot, state: restored }, []));
        const line = frame.marks?.[0];
        assert.equal(line?.type, "packedPolyline");
        assert.ok(Object.is(line.coordinates[0], -0));
    } finally { worker.stop(); }
});

test("the presentation reader fetches and verifies one compatible artifact per digest", async () => {
    const digest = createHash("sha256").update(Buffer.from(moduleBytes, "base64")).digest("hex");
    const manifest = { version: presentationPluginVersion, id: "marker", title: "Marker", definitionIds: ["builtin.beats"],
        observations: [{ id: "bound", port: "min", direction: "input", operation: "waveform", points: 512,
            whenConnected: true, group: "bounds", completeOnly: true }] };
    const entry = { id: "example/marker", provider: "example", format: "wasm",
        schemaVersion: presentationPluginVersion, digest, manifest };
    const fetches: string[] = [];
    const rpc = {
        listPresentations: async () => ({ ready: true, entries: [entry, entry] }),
        fetchPresentation: async (fetchDigest: string) => {
            fetches.push(fetchDigest);
            return { digest: fetchDigest, bytes: moduleBytes };
        },
    };
    const reader = new PresentationArtifactReader();
    const report = (error: string) => assert.fail(error);
    const expected = { ready: true, artifacts: [{ id: entry.id, digest,
        manifest: { ...readPresentationManifest(manifest), ...readWasmPresentationDescriptor(marker) }, bytes: moduleBytes }] };
    assert.deepEqual(await reader.read(rpc, report), expected);
    assert.deepEqual(await reader.read(rpc, report), expected);
    assert.deepEqual(fetches, [digest]);

    const errors: string[] = [];
    const corrupt = { ...rpc, fetchPresentation: async () => ({ digest, bytes: "AA==" }) };
    assert.deepEqual(await new PresentationArtifactReader().read(corrupt, (error) => errors.push(error)),
        { ready: true, artifacts: [] });
    assert.match(errors[0], /digest mismatch/);
});

test("the function ABI records packed geometry and reuses the drawing across worker calls", async () => {
    const instance = createWasmPresentation(moduleBytes);
    const worker = createNodePresentationWorker(moduleBytes);
    try {
        instance.restore("");
        const { frame } = instance.present(wasmPresentationUpdate(snapshot, []));
        const expected = readPresentationFrame(frame);
        assert.deepEqual(expected.marks, [{ id: "2", ink: "output", type: "packedPolyline",
            coordinates: new Float64Array([250, 0, 250, 80]) }]);
        assert.deepEqual(readPresentationFrame(await worker.run(snapshot, [])), expected);
        assert.equal(readPresentationFrame(await worker.run({ ...snapshot, revision: 2 }, [])).reuseDrawing, true);
        worker.restore(null);
        assert.deepEqual(readPresentationFrame(await worker.run(snapshot, [])), expected);
    } finally {
        instance.dispose();
        worker.stop();
    }
});

test("a lane keeps the running WASM session if a replacement fails and restores state on successful replacement", async () => {
    const failures: unknown[] = [];
    const presentation = new ReloadableWasmPresentation(createNodePresentationWorker, (error) => {
        if (error !== undefined) failures.push(error);
    });
    const width = (value: unknown) => {
        const mark = readPresentationFrame(value).marks?.[0];
        assert.equal(mark?.type, "packedPolyline");
        return mark.coordinates[0];
    };
    try {
        const saved = { ...snapshot, state: serializePresentationState({ position: .25 }) };
        assert.equal(width(await presentation.run(moduleBytes, saved, [], () => true)), 125);
        const resized = { ...saved, viewport: { ...snapshot.viewport, width: 400 } };
        assert.equal(width(await presentation.run("AA==", resized, [], () => true)), 100);
        assert.equal(failures.length, 1);
        let readyToSwitch = true;
        const duringGesture = await presentation.run(replacementMarker, resized, [], () => {
            const previous = readyToSwitch;
            readyToSwitch = false;
            return previous;
        });
        assert.equal(readPresentationFrame(duringGesture).reuseDrawing, true);
        assert.equal(width(await presentation.run(replacementMarker, resized, [], () => true)), 100);
    } finally {
        presentation.dispose();
    }
});

test("a reused drawing survives an intervening frame the host received too late to draw", async () => {
    const marks: PresentationFrame["marks"] = [{ type: "packedPolyline", id: "2",
        coordinates: new Float64Array([10, 0, 10, 30]) }];
    let complete!: (value: PresentationFrame) => void;
    let calls = 0;
    const received: { frame: PresentationFrame; draw: boolean }[] = [];
    const session = new PresentationPluginSession(
        () => ++calls === 1 ? new Promise((resolve) => { complete = resolve; }) : { reuseDrawing: true },
        () => {}, (frame, draw) => received.push({ frame, draw }), (error) => { throw error; });
    session.update(snapshot);
    const pending = session.flush();
    session.update({ ...snapshot, viewport: { ...snapshot.viewport, width: 490 } });
    complete({ marks });
    await pending;
    await session.flush();
    assert.deepEqual(received.map(({ draw }) => draw), [false, true]);
    assert.equal(received[1].frame.marks, marks);
});

test("valid maximum-length string controls fit the scene arena", () => {
    const instance = createWasmPresentation(clickDetector.toString("base64"));
    try {
        const inputs = Array.from({ length: 512 }, (): PresentationInput => ({
            type: "control", phase: "commit", id: "a".repeat(160), value: "b".repeat(160),
        }));
        assert.ok(instance.present(wasmPresentationUpdate(snapshot, inputs)).frame);
    } finally { instance.dispose(); }
});

test("Beats closes the pre-history gesture and gives the next preview a fresh ID", () => {
    const instance = createWasmPresentation(wasmFixture("beats").toString("base64"));
    const frame = (inputs: PresentationInput[]) =>
        readPresentationFrame(instance.present(wasmPresentationUpdate({ ...snapshot, editsPending: true }, inputs)).frame);
    const preview = (value: number): PresentationInput => ({ type: "control", id: "bpm", phase: "preview", value });
    try {
        const before = frame([preview(130)]).commands?.[0];
        assert.equal(before?.type, "edit");
        assert.ok(before.gesture);
        const [finish, after] = frame([{ type: "interrupt", reason: "history" }, preview(140)]).commands ?? [];
        assert.equal(finish?.type, "finish");
        assert.equal(finish.gesture, before.gesture);
        assert.equal(after?.type, "edit");
        assert.ok(after.gesture);
        assert.notEqual(after.gesture, before.gesture);
    } finally { instance.dispose(); }
});

test("construction records retain exact signed and unsigned 64-bit values", () => {
    const definitionId = "test.integers";
    const layouts = (["signedInteger", "unsignedInteger"] as const).map((kind, index) => ({
        definitionId, index, layout: compileLayout({ kind, bitWidth: 64 }, { items: 8, bytes: 0 }),
    }));
    const packed = packWasmArguments(layouts, { definitionId, arguments: ["-9223372036854775808", "18446744073709551615"] }, 4096);
    assert.ok(packed);
    const memory = new WebAssembly.Memory({ initial: 1 });
    const base = 8;
    packed.copyTo(memory, base);
    const bytes = new DataView(memory.buffer);
    assert.equal(bytes.getBigInt64(bytes.getUint32(base, true), true), -(1n << 63n));
    assert.equal(bytes.getBigUint64(bytes.getUint32(base + 8, true), true), (1n << 64n) - 1n);
});

test("an initial and an idle presentation frame can request a wake", { timeout: 1000 }, async () => {
    let delivered = 0;
    let inputsSeen = 0;
    let done!: () => void;
    const finished = new Promise<void>((resolve) => { done = resolve; });
    const wake = new PresentationWake(() => session.input({ type: "wake" }));
    const session = new PresentationPluginSession((_, inputs) => {
        inputsSeen += inputs.filter(({ type }) => type === "wake").length;
        return inputsSeen < 2 ? { wakeAfterMs: 0 } : {};
    }, () => { queueMicrotask(() => { void session.flush(); }); }, (frame) => {
        wake.follow(frame);
        if (++delivered === 3) done();
    }, (error) => assert.fail(String(error)));
    try {
        session.update(snapshot, "immediate");
        await finished;
        assert.equal(inputsSeen, 2);
        assert.equal(wake.pending, false);
    } finally {
        wake.cancel();
        session.dispose();
    }
});

test("a rejected construction string restores the budget for the next binding", () => {
    const definitionId = "test.strings";
    const layout = compileLayout({ kind: "string" }, { items: 2, bytes: 0 });
    const packed = packWasmArguments([0, 1].map((index) => ({ definitionId, index, layout })), {
        definitionId, arguments: ["€".repeat(40), "ok"],
    }, 96);
    assert.ok(packed);

    const memory = new WebAssembly.Memory({ initial: 1 });
    const base = 8;
    packed.copyTo(memory, base);
    const view = new DataView(memory.buffer);
    assert.equal(view.getUint32(base, true), 0);
    assert.equal(view.getUint32(base + 4, true), 0);
    assert.equal(view.getUint32(base + 12, true), 2);
    const string = view.getUint32(base + 8, true);
    assert.equal(new TextDecoder().decode(new Uint8Array(memory.buffer,
        view.getUint32(string, true), view.getUint32(string + 4, true))), "ok");
});

test("history retires client gesture handles even when it discards the worker's finish frame", async () => {
    let opened = 0;
    let finished = 0;
    const edits = new PresentationLaneEdits({
        editable: () => true,
        gesture: () => {
            ++opened;
            return { send: async () => {}, finish: async () => { ++finished; } };
        },
        accepted: () => {}, input: () => {}, setError: () => {}, changed: () => {},
        fail: (error) => assert.fail(String(error)),
    });
    let reply!: (frame: PresentationFrame) => void;
    const session = new PresentationPluginSession(() => new Promise((resolve) => { reply = resolve; }),
        () => {}, (frame) => edits.accept(frame.commands), (error) => assert.fail(String(error)));
    session.update(snapshot);

    const event = serializeNodeEvent(SetBeatGrid({ bpm: 133, beats_per_bar: 4, subdivisions: [2] }));
    try {
        for (let index = 0; index < 9; ++index) {
            const gesture = `beats:${index}`;
            session.input({ type: "control", id: "bpm", phase: "preview", value: 133 });
            let frame = session.flush();
            reply({ commands: [{ type: "edit", gesture, event }] });
            await frame;
            assert.equal(opened, index + 1);

            session.input({ type: "control", id: "bpm", phase: "finish" });
            frame = session.flush();
            session.interrupt("history", edits.interrupt());
            reply({ commands: [{ type: "finish", gesture }] });
            await frame;
            assert.equal(finished, index + 1);

            frame = session.flush();
            reply({});
            await frame;
        }
    } finally {
        session.dispose();
        edits.queue.dispose();
        await edits.closeGestures();
    }
});

test("a late edit rejection after history cannot discard a fresh gesture", async () => {
    let reject!: (reason: Error) => void;
    let opened = 0;
    let finished = 0;
    const replies: PresentationInput[] = [];
    const edits = new PresentationLaneEdits({
        editable: () => true,
        gesture: () => ({
            send: ++opened === 1 ? () => new Promise((_, fail) => { reject = fail; }) : async () => {},
            finish: async () => { ++finished; },
        }),
        accepted: () => {}, input: (input) => replies.push(input), setError: () => {}, changed: () => {},
        fail: (error) => assert.fail(String(error)),
    });
    const event = serializeNodeEvent(SetBeatGrid({ bpm: 133, beats_per_bar: 4, subdivisions: [2] }));
    try {
        edits.accept([{ type: "edit", gesture: "old", requestId: "old-value", event }]);
        assert.deepEqual(edits.interrupt(), [{ type: "editResult", requestId: "old-value", error: "Presentation edit interrupted" }]);
        edits.accept([{ type: "edit", gesture: "new", requestId: "new-value", event }]);
        reject(new Error("Undo closed the old gesture"));
        await new Promise<void>((resolve) => setImmediate(resolve));
        assert.equal(opened, 2);
        assert.equal(finished, 1);
        assert.deepEqual(replies, [{ type: "editResult", requestId: "new-value", acceptedRevision: undefined }]);
    } finally {
        edits.queue.dispose();
        await edits.closeGestures();
    }
});

test("an interrupted edit still publishes the revision it committed", async () => {
    let commit!: () => void;
    const revisions: number[] = [];
    const replies: PresentationInput[] = [];
    const edits = new PresentationLaneEdits({
        editable: () => true,
        gesture: () => ({ send: () => new Promise<void>((resolve) => { commit = resolve; }),
            finish: async () => {}, acceptedRevision: () => 2 }),
        accepted: (revision) => revisions.push(revision), input: (input) => replies.push(input),
        setError: () => {}, changed: () => {}, fail: (error) => assert.fail(String(error)),
    });
    const event = serializeNodeEvent(SetBeatGrid({ bpm: 133, beats_per_bar: 4, subdivisions: [2] }));
    try {
        edits.accept([{ type: "edit", gesture: "bpm-drag", requestId: "bpm-value", event }]);
        assert.deepEqual(edits.interrupt(), [{ type: "editResult", requestId: "bpm-value", error: "Presentation edit interrupted" }]);
        commit();
        await new Promise<void>((resolve) => setImmediate(resolve));
        assert.deepEqual(revisions, [2]);
        assert.deepEqual(replies, []);
    } finally {
        edits.queue.dispose();
        await edits.closeGestures();
    }
});

test("a constructor sequence that exhausts the item budget leaves room for later bindings", () => {
    const definitionId = "test.budget";
    const budget = { items: 8, bytes: 0 };
    const sequence = compileLayout({ kind: "sequence", element: { kind: "aggregate",
        fields: [{ name: "enabled", value: { kind: "boolean" } }] } }, budget);
    const boolean = compileLayout({ kind: "boolean" }, budget);
    const packed = packWasmArguments([{ definitionId, index: 0, layout: sequence },
        { definitionId, index: 1, layout: boolean }], {
        definitionId, arguments: [Array(65536).fill({ enabled: true }), true],
    }, 1024 * 1024);
    assert.ok(packed);

    const memory = new WebAssembly.Memory({ initial: 1 });
    const base = 8;
    packed.copyTo(memory, base);
    const view = new DataView(memory.buffer);
    assert.equal(view.getUint32(base + 4, true), 0);
    assert.equal(view.getUint32(base + 12, true), 2);
    assert.equal(view.getUint8(view.getUint32(base + 8, true)), 1);
});

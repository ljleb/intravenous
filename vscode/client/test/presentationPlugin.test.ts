import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { openPresentationTimelineReader, renderPresentationTimelineGrid } from "../src/presentationTimelineGrid";
import { SetBeatGrid } from "../src/generated/beatsEvents";
import { PlacePoint } from "../src/generated/automationEvents";
import { serializeNodeEvent } from "../src/nodeEvents";
import { PresentationEdits, type PresentationEdit } from "../src/presentationEdits";
import { PresentationLaneEdits } from "../src/presentationLaneEdits";
import { presentationPluginVersion, readPresentationFrame, readPresentationManifest,
    type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "../src/presentationPlugin";
import { PresentationPluginSession } from "../src/presentationPluginSession";
import { PresentationArtifactReader } from "../src/presentationArtifacts";
import { createWasmPresentation, readWasmPresentationDescriptor, wasmPresentationUpdate } from "../src/wasmPresentation";
import { createNodePresentationWorker } from "../src/nodeWasmPresentationWorker";
import { ReloadableWasmPresentation } from "../src/reloadableWasmPresentation";

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
    const sent: PresentationEdit[] = [];
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
    const sent: PresentationEdit[] = [];
    const edits = new PresentationEdits(async (command) => { sent.push(command); }, () => {},
        (error) => assert.fail(String(error)), { set: () => 1, clear: () => {} });
    const eventType = { nominalId: "test.edit", definitionFingerprint: "v1" };
    const edit = (payload: number, replacePending?: true): PresentationEdit =>
        ({ type: "edit", gesture: "test", replacePending, event: { eventType, payload } });
    const commands = [edit(1, true), edit(2), edit(3, true), { type: "finish", gesture: "test" } as const];
    for (const command of commands) edits.enqueue(command);
    await new Promise<void>((resolve) => { setImmediate(resolve); });
    assert.deepEqual(sent, commands);
});

test("a WASM placement draws while its node event is pending and resumes on its matching reply", async () => {
    const worker = createNodePresentationWorker(wasmFixture("placement").toString("base64"));
    const curve = { ...snapshot, node: { ...snapshot.node, definitionId: "builtin.automation.curve", arguments: [] } };
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
        const initial = await run(drag("begin", 40));
        const insert = initial.commands?.[0];
        assert.equal(insert?.type, "edit");
        assert.ok(insert.requestId);
        edits.accept(initial, () => {});
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
        edits.accept(moved, () => {});
        accept();
        await turn();
        const end = await run(...replies.splice(0));
        assert.equal(end.commands?.[0]?.type, "finish");
        edits.accept(end, () => {});
        await turn();
        assert.equal((await run(...replies.splice(0))).status, "Placement pending");
        assert.equal(readPresentationFrame(await worker.run({ ...curve, revision }, [])).status, "Placement accepted");
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
    const accepted: Array<{ state: PresentationFrame["commands"]; draw: boolean; hasInput: boolean }> = [];
    const errors: unknown[] = [];
    const session = new PresentationPluginSession((view, inputs) => {
        calls.push({ width: view.viewport.width, inputs });
        return new Promise((resolve) => replies.push(resolve));
    }, () => {}, (frame, draw, inputs) => accepted.push({ state: frame.commands, draw, hasInput: inputs.length > 0 }), (error) => errors.push(error));
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
    const commands: PresentationFrame["commands"] = [{ type: "state", value: { x: 5 } }];
    replies[0]({ commands });
    await first;
    assert.deepEqual(accepted, [{ state: commands, draw: false, hasInput: true }]);

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
    const session = new PresentationPluginSession(async () => ({ commands: [{ type: "state", value: NaN }] }),
        () => {}, () => assert.fail("Published an invalid frame"), (error) => errors.push(error));
    session.update(snapshot);
    await session.flush();
    assert.match(String(errors[0]), /Invalid presentation frame/);
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
    replies[0]({ commands: [{ type: "finish", gesture: "bpm" }] });
    await inFlight;
    const interruption = session.flush();
    replies[1]({});
    await interruption;
    assert.deepEqual(inputs, [[control], [{ type: "interrupt", reason: "history" }]]);
    assert.deepEqual(accepted, [[{ type: "interrupt", reason: "history" }]]);
    session.dispose();
});

const marker = wasmFixture("marker");
const moduleBytes = marker.toString("base64");

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
        presentation.restore({ position: .25 });
        assert.equal(width(await presentation.run(moduleBytes, snapshot, [], () => true)), 125);
        const resized = { ...snapshot, viewport: { ...snapshot.viewport, width: 400 } };
        assert.equal(width(await presentation.run("AA==", resized, [], () => true)), 100);
        assert.equal(failures.length, 1);
        // Add a custom section so this is a distinct, compatible artifact.
        const replacement = Buffer.concat([marker, Buffer.from([0, 2, 1, 120])]).toString("base64");
        let readyToSwitch = true;
        const duringGesture = await presentation.run(replacement, resized, [], () => {
            const previous = readyToSwitch;
            readyToSwitch = false;
            return previous;
        });
        assert.equal(readPresentationFrame(duringGesture).reuseDrawing, true);
        assert.equal(width(await presentation.run(replacement, resized, [], () => true)), 100);
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

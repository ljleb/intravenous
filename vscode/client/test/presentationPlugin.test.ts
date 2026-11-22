import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { createBeatsPresentation } from "../src/beatsPresentationPlugin";
import { PresentationEdits, type PresentationEdit } from "../src/presentationEdits";
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

const clickDetector = readFileSync(resolve(__dirname,
    "../../../../src/intravenous/builtin_packages/builtin/presentations/click_detector.wasm"));
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
            ...snapshot, observations: { clicks: { status: "pending", result,
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
            observations: { clicks: { status: "ready" as const, result,
                spans: [{ begin: "0", end: "96000", windowOffset: "0", result }] } },
        };
        const frame = readPresentationFrame(instance.present(wasmPresentationUpdate(displayed, [])).frame);
        assert.equal(frame.status, "1000000 recorded clicks in view");
        assert.equal(frame.marks?.length, 500);
        assert.ok(frame.marks?.every(({ type }) => type === "packedPolyline"));

        const clipped = { ...displayed, observations: { clicks: { status: "pending" as const, result,
            spans: [{ begin: "1", end: "192", windowOffset: "0", result }] } } };
        assert.equal(instance.present(wasmPresentationUpdate(clipped, [])).frame?.status, "At least 1 recorded click in view");

        const waiting = { ...snapshot, node: { ...snapshot.node,
            ports: [{ direction: "input" as const, kind: "continuous" as const, connected: true, name: "audio" }] } };
        assert.equal(instance.present(wasmPresentationUpdate(waiting, [])).frame?.status, "Waiting for click observations");
        assert.equal(instance.present(wasmPresentationUpdate(snapshot, [])).frame?.status, "Connect the audio to inspect");
    } finally { instance.dispose(); }
});

test("Beats uses the portable contract for rendering, settings and exact seek marks", () => {
    const present = createBeatsPresentation();
    const first = readPresentationFrame(present(snapshot, []));
    assert.equal(first.panel?.label, "120 BPM");
    assert.ok(first.seekMarks?.includes("24000"));
    assert.equal(first.marks?.find(({ id }) => id === "beat:24000")?.type, "polyline");

    const second = readPresentationFrame(present(snapshot, [
        { type: "control", id: "bpm", phase: "preview", value: 140 },
        { type: "control", id: "bpm", phase: "preview", value: 150 },
        { type: "control", id: "bpm", phase: "finish" },
    ]));
    assert.equal(second.panel?.label, "150 BPM");
    const edit = second.commands?.[0];
    assert.equal(second.commands?.length, 2);
    assert.equal(edit?.type, "edit");
    assert.equal(edit.gesture, "bpm");
    assert.equal(edit.replacePending, true);
    assert.deepEqual(edit.event.payload, { bpm: 150, beats_per_bar: 4, subdivisions: [2] });
    assert.deepEqual(second.commands[1], { type: "finish", gesture: "bpm" });
    assert.equal(present({ ...snapshot, editsPending: true }, []).panel?.label, "150 BPM");
    const read = present.openSeekReader?.();
    assert.ok(read?.({ first: "96000", length: "96000", sampleRate: 48000, width: 500 }).includes("115200"));
    assert.equal(present(snapshot, []).panel?.label, "120 BPM");
    assert.deepEqual(present({ ...snapshot, editable: false }, [
        { type: "control", id: "bpm", phase: "commit", value: 150 },
    ]).commands, []);
});

test("full-state Beats previews reach the backend during a drag and flush the latest value at release", async () => {
    const present = createBeatsPresentation();
    const preview = (bpm: number) => {
        const command = present(snapshot, [{ type: "control", id: "bpm", phase: "preview", value: bpm }]).commands?.[0];
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

    const finish = { type: "finish", gesture: "bpm" } as const;
    edits.enqueue(preview(170));
    edits.enqueue(c);
    edits.enqueue(finish);
    await turn();
    assert.equal(alarm, undefined);
    assert.equal(edits.pending, false);
    assert.deepEqual(sent, [a, b, c, finish]);
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

test("a synchronous presentation draws each new viewport without waiting for another frame", () => {
    const accepted: PresentationFrame[] = [];
    const session = new PresentationPluginSession(createBeatsPresentation(),
        () => assert.fail("Viewport update queued another frame"),
        (frame, draw) => { assert.equal(draw, true); accepted.push(frame); }, (error) => assert.fail(String(error)));
    const beat = (frame: PresentationFrame) => frame.marks?.find(({ id }) => id === "beat:24000");

    session.update(snapshot, "immediate");
    assert.equal(accepted.length, 1);
    const first = beat(accepted[0]);
    assert.equal(first?.type, "polyline");
    assert.deepEqual(first.points.at(-1), { x: 125, y: 80 });

    session.update({ ...snapshot, viewport: { ...snapshot.viewport, height: 160,
        time: { beginSample: "0", beginFraction: 0, spanSamples: 48000, sampleRate: 48000 } } }, "immediate");
    assert.equal(accepted.length, 2);
    const zoomed = beat(accepted[1]);
    assert.equal(zoomed?.type, "polyline");
    assert.deepEqual(zoomed.points.at(-1), { x: 250, y: 160 });
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
    assert.throws(() => readPresentationFrame({ seekMarks: ["9223372036854775808"] }), /Invalid presentation frame/);
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

const marker = readFileSync(resolve(__dirname,
    "../../../../examples/wasm-presentation/packages/marker/presentations/marker.wasm"));
const moduleBytes = marker.toString("base64");

test("the presentation reader fetches and verifies one compatible artifact per digest", async () => {
    const digest = createHash("sha256").update(Buffer.from(moduleBytes, "base64")).digest("hex");
    const manifest = { version: presentationPluginVersion, id: "marker", title: "Marker", definitionIds: ["builtin.beats"] };
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

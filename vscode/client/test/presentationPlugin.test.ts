import assert from "node:assert/strict";
import { mkdir, mkdtemp, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { runInNewContext } from "node:vm";
import { createBeatsPresentation } from "../src/beatsPresentationPlugin";
import { PresentationEdits, type PresentationEdit } from "../src/presentationEdits";
import { readPresentationFrame, type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "../src/presentationPlugin";
import { PresentationPluginSession } from "../src/presentationPluginSession";
import { readProjectPresentations } from "../src/projectPresentationPlugins";

const snapshot: PresentationSnapshot = {
    node: { definitionId: "builtin.beats", arguments: [120, 4, [2]], ports: [] },
    revision: 1, mode: "editable", editable: true, viewport: { width: 500, height: 80,
        time: { beginSeconds: 0, spanSeconds: 2, sampleRate: 48000 } }, observations: {}, state: null,
};

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
        time: { beginSeconds: 0, spanSeconds: 1, sampleRate: 48000 } } }, "immediate");
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
    }, () => {}, (frame, draw, hasInput) => accepted.push({ state: frame.commands, draw, hasInput }), (error) => errors.push(error));
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

test("a project script can be discovered and run with the same input data in a headless host", async () => {
    const project = await mkdtemp(join(tmpdir(), "iv-presentations-"));
    const packages = join(project, "packages");
    const directory = join(packages, "marker", "presentations");
    const other = join(packages, "other", "presentations");
    try {
        await mkdir(directory, { recursive: true });
        const manifest = { version: 1, id: "example.marker", title: "Marker", definitionIds: ["builtin.beats"] };
        const script = `function present(snapshot, inputs) {
            const move = inputs.findLast((input) => input.type === "gesture" && input.phase === "move");
            const x = move?.point.x ?? snapshot.state?.x ?? 10;
            return { marks: [{ id: "marker-line", type: "polyline", points: [{ x, y: 0 }, { x, y: snapshot.viewport.height }] }],
                commands: move ? [{ type: "state", value: { x } }] : [] };
        }`;
        await writeFile(join(directory, "marker.json"), JSON.stringify(manifest));
        await writeFile(join(directory, "marker.js"), script);
        await writeFile(join(directory, "unsupported.json"), JSON.stringify({ ...manifest, version: 2 }));
        await writeFile(join(directory, "linked.json"), JSON.stringify({ ...manifest, id: "example.linked" }));
        await symlink("marker.js", join(directory, "linked.js"));
        await mkdir(other, { recursive: true });
        await writeFile(join(other, "another.json"), JSON.stringify({ ...manifest, id: "example.another" }));
        await writeFile(join(other, "another.js"), script);
        await writeFile(join(other, "duplicate.json"), JSON.stringify(manifest));
        await writeFile(join(other, "duplicate.js"), "function present() { return {}; }");
        await symlink(join(packages, "marker"), join(packages, "linked-package"), "dir");
        const errors: string[] = [];
        const [plugin, ...others] = await readProjectPresentations(project, (error) => errors.push(error));
        assert.deepEqual(others.map(({ manifest }) => manifest.id), ["example.another"]);
        assert.equal(plugin.manifest.id, manifest.id);
        assert.equal(errors.length, 3);
        assert.match(errors.join("\n"), /linked\.json.*regular file/);
        assert.match(errors.join("\n"), /unsupported\.json.*unsupported/);
        assert.match(errors.join("\n"), /packages[\\/]other[\\/]presentations[\\/]duplicate\.json.*Duplicate presentation ID example\.marker/);
        const result = runInNewContext(`${plugin.source}\npresent(snapshot, inputs)`, { snapshot, inputs: [
            { type: "gesture", phase: "move", id: "handle", point: { x: 17, y: 4 },
                modifiers: { shift: false, control: false, alt: false, meta: false } },
        ] }, { timeout: 100 });
        const frame = readPresentationFrame(structuredClone(result));
        assert.deepEqual(frame.commands, [{ type: "state", value: { x: 17 } }]);
        assert.deepEqual(frame.marks?.[0], { id: "marker-line", type: "polyline", points: [{ x: 17, y: 0 }, { x: 17, y: 80 }] });
    } finally {
        await rm(project, { recursive: true, force: true });
    }
});

import test from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs/promises";
import * as path from "node:path";
import * as os from "node:os";
import { reactive } from "vue";
import { readAutomationCurveView, zoomAutomationValueAxis } from "../src/automationCurveGeometry";
import { saveGraphLayoutMessage } from "../src/graphEditorProtocol";
import { GraphLayoutStore } from "../src/graphLayoutStore";
import { addressKey, copyLanePresentationStates, placeNodes, presentationStateKey, readLayout } from "../src/graphScope";
import type { TimelineSnapMemory } from "../src/timelineSnap";

test("graph layout saves serialize, survive a new store, and leave project intent untouched", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const project = path.join(directory, "iv_project.jsonl");
        await fs.writeFile(project, "project intent\n");
        const store = new GraphLayoutStore(directory);
        assert.deepEqual(await store.load(), readLayout(null));
        const first = readLayout({ positions: { node: { x: 2, y: 3 } } });
        const key = addressKey({ instanceId: "curve" });
        const last = readLayout({ positions: { node: { x: 200, y: -300 } }, viewport: { x: 10, y: 20, scale: 0.5 },
            automationBounds: { [key]: { min: -2 }, inverted: { min: 1, max: 0 } },
            timeline: { beginSeconds: -5, secondsPerPixel: 0.02, laneHeight: 640, scrollTop: 123 } });
        await Promise.all([store.save(first), store.save(last)]);
        assert.deepEqual(await new GraphLayoutStore(directory).load(), last);
        assert.deepEqual(placeNodes([], last).automationBounds, { [key]: { min: -2 } });
        assert.equal(await fs.readFile(project, "utf8"), "project intent\n");
        assert.deepEqual(await fs.readdir(path.dirname(store.filePath)), ["graph-layout.json"]);
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("the project layout round trips lane order and independent presentation states", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const store = new GraphLayoutStore(directory);
        const layout = readLayout(null);
        const address = { instanceId: "curve" };
        const curve = presentationStateKey(address, "iv.builtin.automation.curve");
        const custom = presentationStateKey(address, "example.custom");
        const first = { ruler: "half", display: "cropped", pan: -2.5, zoom: { factor: 4, center: .5 } } as const;
        const second = { cursor: [2, 4], layers: { points: true, preview: false } };
        const order = [addressKey(address), addressKey({ instanceId: "another" })];
        const presentationStates = { [curve]: first, [custom]: second };
        await Promise.all([store.save(layout, { [curve]: first }, order), store.save(layout, { [custom]: second })]);
        const reloaded = placeNodes([], await new GraphLayoutStore(directory).load());
        assert.deepEqual(reloaded.presentationStates, presentationStates);
        assert.deepEqual(reloaded.laneOrder, order);
        const written = JSON.parse(await fs.readFile(store.filePath, "utf8"));
        assert.deepEqual(written.presentationStates, presentationStates);
        assert.deepEqual(written.laneOrder, order);
        await store.save(layout, { [curve]: null });
        assert.deepEqual((await store.load()).presentationStates, { [custom]: second });
        assert.deepEqual((await store.load()).laneOrder, order);
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("lane copies save independent snapshots of all presentation states owned by the selected instances", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-lane-copies-"));
    try {
        const store = new GraphLayoutStore(directory);
        const memberPath = [{ name: "member" }];
        const curve = (instanceId: string) => presentationStateKey({ instanceId }, "iv.builtin.automation.curve");
        const member = (instanceId: string) => presentationStateKey({ instanceId, path: memberPath }, "example.member");
        const zoom = { factor: 2, center: .5 };
        const layout = reactive(readLayout({ presentationStates: {
            [curve("curve")]: { ruler: "half", pan: 3, zoom },
            [member("module")]: { layers: { preview: true } },
            [curve("unselected")]: { pan: 4 },
        } }));
        await store.save(layout, layout.presentationStates);
        const snapshots = copyLanePresentationStates(layout.presentationStates, [
            { sourceInstanceId: "curve", instanceId: "curve-copy" },
            { sourceInstanceId: "module", instanceId: "module-copy" },
            { sourceInstanceId: "without-settings", instanceId: "default-copy" },
        ]);
        zoom.factor = 8;
        const message = structuredClone(saveGraphLayoutMessage(layout, snapshots, false));
        assert.deepEqual(Object.keys(message.presentationStates).sort(), [curve("curve-copy"), member("module-copy")].sort());

        const later = { ...layout.presentationStates[curve("curve")], pan: 42 };
        await Promise.all([
            store.save(layout, { [curve("curve")]: later }),
            store.save(readLayout(message.layout), message.presentationStates),
        ]);
        const { presentationStates } = await new GraphLayoutStore(directory).load();
        assert.deepEqual(presentationStates[curve("curve-copy")], { ruler: "half", pan: 3, zoom: { factor: 2, center: .5 } });
        assert.deepEqual(presentationStates[curve("curve")], later);
        assert.deepEqual(presentationStates[member("module-copy")], { layers: { preview: true } });
        assert.deepEqual(presentationStates[curve("unselected")], { pan: 4 });
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("viewport saves preserve snap settings written before a graph transaction", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-timeline-snap-"));
    try {
        const store = new GraphLayoutStore(directory);
        const memory: TimelineSnapMemory = { beats: [95, 3, [2, 3]] };
        await Promise.all([
            store.prepareTimelineSnap(() => ({ memory })),
            store.save(readLayout({ timeline: { beginSeconds: 5 } })),
        ]);
        const reloaded = new GraphLayoutStore(directory);
        assert.equal((await reloaded.load()).timeline.beginSeconds, 5);
        const saved = await reloaded.prepareTimelineSnap((memory) => ({ memory }));
        assert.deepEqual(saved.memory.beats, [95, 3, [2, 3]]);
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("zoom saves from the reactive graph layout survive the message channel and reload", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const store = new GraphLayoutStore(directory);
        const address = { instanceId: "curve" };
        const curve = presentationStateKey(address, "iv.builtin.automation.curve");
        const custom = presentationStateKey(address, "example.custom");
        const otherState = { layers: { points: true } };
        await store.save(readLayout(null), { [curve]: { pan: 3 }, [custom]: otherState });

        const layout = reactive(await store.load());
        const beforeZoom = readAutomationCurveView(layout.presentationStates[curve]);
        const afterZoom = readAutomationCurveView({ ...beforeZoom, ...zoomAutomationValueAxis(beforeZoom, 4, .625) });
        assert.equal(afterZoom.zoom?.factor, 4);
        layout.presentationStates[curve] = afterZoom;
        const message = structuredClone(saveGraphLayoutMessage(layout, { [curve]: afterZoom }, false));
        await store.save(readLayout(message.layout), message.presentationStates, message.laneOrder);

        const reloaded = await new GraphLayoutStore(directory).load();
        assert.deepEqual(readAutomationCurveView(reloaded.presentationStates[curve]), afterZoom);
        assert.deepEqual(reloaded.presentationStates[custom], otherState);
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("an unreadable layout is reported instead of silently replacing user data", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const store = new GraphLayoutStore(directory);
        await fs.mkdir(path.dirname(store.filePath));
        await fs.writeFile(store.filePath, "{broken");
        await assert.rejects(store.load(), SyntaxError);
        assert.equal(await fs.readFile(store.filePath, "utf8"), "{broken");
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

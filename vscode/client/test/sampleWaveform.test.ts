import { receiveSnapshot, type ObservationSnapshotFixture } from "./observationFixtures";
import assert from "node:assert/strict";
import test from "node:test";
import { sampleWindow, sampleWaveform } from "../src/sampleWaveform";
import { ObservationValues, type ObservationWindowResult } from "../src/observations";

function result(): ObservationWindowResult {
    return { id: "wave", window: { begin: "0", length: "10", points: 10 }, status: "ready",
        approximate: true, semanticVersion: "1", dataVersion: "4", paceExponent: 0,
        regions: [{ begin: "0", length: "3", points: [
            { index: "0", available: true, value: { value: 0 } },
            { index: "1", available: true, value: { value: .5 } },
            { index: "2", available: false },
        ] }, { begin: "7", length: "2", points: [
            { index: "7", available: true, value: { value: -.5 } },
            { index: "8", available: true, value: { value: 0 } },
        ] }] };
}
test("waveform requests quantize to project samples and bound the representative point count", () => {
    assert.deepEqual(sampleWindow(-.5, .5, 48000, 2000), { begin: "-24000", length: "48000", points: 1024 });
    assert.equal(sampleWindow(0, 1, null, 512), undefined);
    assert.equal(sampleWindow(1, 0, 48000, 512), undefined);
    assert.equal(sampleWindow(Infinity, 0, 48000, 512), undefined);
});
test("waveforms keep separate paths for gaps, unavailable points and overlapping Regions", () => {
    const data = result();
    const geometry = sampleWaveform(data);
    assert.equal(geometry.paths.length, 2);
    assert.equal(geometry.paths[0], "M0.000,50.000L100.000,27.500");
    assert.equal(geometry.paths[1], "M700.000,72.500L800.000,50.000");
    assert.deepEqual(geometry.coverage, [{ x: 0, width: 300 }, { x: 700, width: 200 }]);
    data.regions.push(data.regions[1]);
    assert.equal(sampleWaveform(data).paths.length, 3);
    data.status = "pending";
    assert.deepEqual(sampleWaveform(data), { paths: [], coverage: [] });
});
test("window results reject obsolete viewport positions and data versions without live probes", () => {
    const values = new ObservationValues();
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const data = result();
    values.expectWindows([{ id: "wave", source, operation: "waveform", window: data.window }]);
    const update = (window: ObservationWindowResult): ObservationSnapshotFixture => ({
        activationId: "0", generation: "7", revision: "0", index: "0", blockSize: 0, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    });
    assert.equal(receiveSnapshot(values, update(data)), true);
    assert.equal(values.windowForDisplay("wave")?.dataVersion, "4");
    receiveSnapshot(values, update({ ...data, dataVersion: "3" }));
    assert.equal(values.windowForDisplay("wave")?.dataVersion, "4");
    const next = { ...data.window, begin: "100" };
    values.expectWindows([{ id: "wave", source, operation: "waveform", window: next }]);
    assert.equal(values.windowForDisplay("wave"), undefined);
    receiveSnapshot(values, update(data));
    assert.equal(values.windowForDisplay("wave"), undefined);
    receiveSnapshot(values, update({ ...data, window: next, regions: [] }));
    assert.deepEqual(values.windowForDisplay("wave")?.window, next);
    receiveSnapshot(values, { ...update({ ...data, window: next }), generation: "8", scopes: [] });
    receiveSnapshot(values, update({ ...data, window: next }));
    assert.equal(values.windowForDisplay("wave")?.outdated, true);
});
test("window previews reproject the last ready result while an overlapping viewport loads", () => {
    const values = new ObservationValues();
    const data = result();
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const request = { id: "wave", source, operation: "waveform", window: data.window };
    const receive = (window: ObservationWindowResult, generation = "7") => receiveSnapshot(values, {
        activationId: "0", generation, revision: "0", index: "0", blockSize: 0, dataVersion: "4",
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    });
    values.expectWindows([request]);
    receive(data);
    const next = { ...data.window, begin: "6" };
    assert.equal(values.windowForDisplay("wave", next)?.outdated, true);
    values.expectWindows([{ ...request, window: next }]);
    receive({ ...data, window: next, status: "pending", regions: [] });
    const preview = values.windowForDisplay("wave");
    assert.equal(preview?.outdated, true);
    assert.equal(sampleWaveform(preview, undefined, next).paths[1], "M100.000,72.500L200.000,50.000");
    assert.equal(values.windowForDisplay("wave", { ...next, begin: "10" }), undefined);
    receive({ ...data, window: next, regions: [] });
    assert.deepEqual(values.windowForDisplay("wave")?.regions, []);
    values.expectWindows([{ ...request, source: { ...source, member: "another" }, window: next }]);
    assert.equal(values.windowForDisplay("wave"), undefined);
    receive({ ...data, window: next });
    receive({ ...data, window: next, status: "pending", regions: [] }, "8");
    assert.equal(values.windowForDisplay("wave")?.outdated, true);
});

test("a ready window overlaps a new request across the signed timeline seam", () => {
    const values = new ObservationValues();
    const seam = 1n << 63n;
    const data = { ...result(), window: { begin: String(seam - 8n), length: "32", points: 16 } };
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const request = { id: "wave", source, operation: "waveform", window: data.window };
    values.expectWindows([request]);
    receiveSnapshot(values, { activationId: "0", generation: "7", revision: "0", index: "0", blockSize: 0, dataVersion: "4",
        scopes: [{ scopeId: "viewport", values: [], windows: [data] }] });
    const next = { ...data.window, begin: String(5n - seam) };
    values.expectWindows([{ ...request, window: next }]);
    assert.equal(values.windowForDisplay("wave")?.outdated, true);
});

test("bound windows are usable as soon as their execution frame arrives", () => {
    const values = new ObservationValues();
    const data = result();
    const bounds = ["min", "max"] as const;
    values.expectWindows(bounds.map((id) => ({ id, window: data.window, operation: "waveform",
        source: { node: { instanceId: "curve" }, kind: "input", member: id } })));
    const update: ObservationSnapshotFixture = {
        activationId: "1", generation: "8", revision: "0", index: "0", blockSize: 0, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows: bounds.map((id) => ({ ...data, id })) }],
    };
    assert.equal(receiveSnapshot(values, update), true);
    for (const id of bounds) assert.deepEqual(values.windowForDisplay(id)?.regions, data.regions);
});

test("window display retains the last compatible samples across graph refreshes", () => {
    const values = new ObservationValues();
    const data = { ...result(), sampleRate: 48000 };
    const source = { node: { instanceId: "curve" }, kind: "input" as const, member: "max" };
    const request = { id: data.id, source, operation: "waveform", window: data.window };
    const publish = (generation: string, window: ObservationWindowResult, sampleRate = 48000) => receiveSnapshot(values, {
        activationId: "1", generation, revision: "0", index: "0", blockSize: 64, dataVersion: "0", sampleRate,
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    });
    values.expectWindows([request]);
    publish("7", data);
    const geometry = sampleWaveform(data);

    publish("8", { ...data, status: "pending", regions: [] });
    assert.deepEqual(sampleWaveform(values.windowForDisplay(data.id)), geometry);
    assert.equal(values.windowForDisplay(data.id)?.outdated, true);
    publish("8", { ...data, status: "failed", error: "Snapshot changed", regions: [] });
    assert.deepEqual(sampleWaveform(values.windowForDisplay(data.id)), geometry);
    assert.equal(values.windowForDisplay(data.id)?.error, "Snapshot changed");
    assert.equal(publish("7", { ...data, dataVersion: "30", regions: [] }), false);

    publish("8", { ...data, dataVersion: "1", regions: [] });
    assert.deepEqual(sampleWaveform(values.windowForDisplay(data.id)).paths, []);
    assert.equal(values.windowForDisplay(data.id)?.dataVersion, "1");
    publish("9", data, 44100);
    assert.equal(values.windowForDisplay(data.id), undefined);
    publish("9", { ...data, sampleRate: 44100 }, 44100);
    assert.equal(values.windowForDisplay(data.id)?.sampleRate, 44100);
    publish("9", { ...data, status: "unavailable", regions: [] }, 44100);
    assert.equal(values.windowForDisplay(data.id)?.status, "unavailable");
    publish("10", { ...data, sampleRate: 44100 }, 44100);
    values.clear();
    assert.equal(values.windowForDisplay(data.id), undefined);
});

test("a returning viewport keeps its fine samples across demotion and later windows", () => {
    const values = new ObservationValues();
    const data = result();
    const source = { node: { instanceId: "curve" }, kind: "input" as const, member: "max" };
    const request = { id: data.id, source, operation: "waveform", window: data.window };
    const publish = (window: ObservationWindowResult) => receiveSnapshot(values, {
        activationId: "1", generation: "1", revision: "0", index: "0", blockSize: 64, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    });
    values.expectWindows([request]);
    publish(data);
    for (let offset = 1; offset <= 4; ++offset) {
        const window = { ...data.window, begin: String(BigInt(data.window.begin) + BigInt(offset) * BigInt(data.window.length)) };
        values.expectWindows([{ ...request, window }]);
        publish({ ...data, window, regions: [] });
    }
    values.expectWindows([]);
    assert.equal(values.windowForDisplay(data.id), undefined);
    values.expectWindows([request]);
    publish({ ...data, status: "pending", regions: [] });
    assert.deepEqual(values.windowForDisplay(data.id)?.regions, data.regions);
    assert.equal(values.windowForDisplay(data.id)?.outdated, true);
    values.retireWindow(data.id);
    values.expectWindows([{ ...request, source: { ...source, member: "min" } }]);
    assert.equal(values.windowForDisplay(data.id), undefined);
});

test("the retained payload budget evicts history while preserving current readings", () => {
    const values = new ObservationValues();
    const data = result();
    const marks = Array.from({ length: 1024 }, () => ({ index: data.window.begin, depth: 0 }));
    const windows: ObservationWindowResult[] = Array.from({ length: 66 }, (_, index) => ({ ...data,
        id: `window:${index}`, regions: [], marks }));
    const interests = windows.map(({ id, window }) => ({ id, window, operation: "beat-grid", source: {
        node: { instanceId: id }, kind: "input" as const, member: "beats", portKind: "event" as const,
    } }));
    values.expectWindows(interests);
    receiveSnapshot(values, { activationId: "1", generation: "1", revision: "0", index: "0", blockSize: 64, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows }] });
    assert.equal(values.readyWindowsForDisplay([interests[0].id], data.window).length, 1);
    values.expectWindows([]);
    assert.equal(values.readyWindowsForDisplay([interests[0].id], data.window).length, 0);
    assert.equal(values.readyWindowsForDisplay([interests.at(-1)!.id], data.window).length, 1);
});
test("viewport Tock interests carry windows and panning never requests live observation operations", async () => {
    const { PresentationObservations } = await import("../src/presentationObservations");
    const requests: import("../src/observations").ObservationHostRequest[] = [];
    const scope = new PresentationObservations((request) => {
        requests.push(request);
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [
                { nodeIndex: 0, kind: "output", member: "out", delivery: "window", operations: ["waveform"],
                    channels: [{ index: 0, name: "center" }] },
            ] } : { revision: "0" } }));
    });
    try {
        const owner = Symbol(), node = { instanceId: "tock" };
        const window = { begin: "0", length: "1000", points: 200 };
        scope.node(owner, node, "prefetch", window);
        await new Promise((resolve) => setTimeout(resolve, 250));
        const nearby = requests.at(-1);
        assert.ok(nearby?.type === "observationReconcile");
        assert.equal(nearby.interests.length, 2);
        assert.ok(nearby.interests.every((entry) => entry.prefetch && entry.window && entry.window.points < window.points));

        scope.node(owner, node, "visible", window);
        await new Promise((resolve) => setTimeout(resolve, 250));
        const first = requests.at(-1);
        assert.ok(first?.type === "observationReconcile");
        assert.equal(first.interests.filter(({ prefetch }) => !prefetch).length, 1);
        assert.equal(first.interests.filter(({ prefetch }) => prefetch).length, 6);
        assert.equal(first.interests[0].operation, "waveform");
        assert.deepEqual(first.interests[0].window, window);
        for (const preview of nearby.interests) assert.deepEqual(first.interests.find(({ id }) => id === preview.id), preview);
        assert.equal(requests.filter(({ type }) => type === "observationDescribe").length, 1);
        scope.node(owner, node, "visible", { ...window, begin: "1000" });
        await new Promise((resolve) => setTimeout(resolve, 250));
        const next = requests.at(-1);
        assert.ok(next?.type === "observationReconcile");
        assert.equal(next.interests[0].id, first.interests[0].id);
        assert.equal(next.interests[0].window?.begin, "1000");
    } finally { scope.dispose(); }
});

test("downsampled waveform positions retain their phase relative to the project sample window", () => {
    const data = result();
    data.window = { begin: "1", length: "4", points: 4 };
    data.paceExponent = -1;
    data.regions = [{ begin: "1", length: "2", points: [
        { index: "1", available: true, value: { value: 0 } },
        { index: "2", available: true, value: { value: .5 } },
    ] }];
    assert.deepEqual(sampleWaveform(data).paths, ["M250.000,50.000L750.000,27.500"]);
});

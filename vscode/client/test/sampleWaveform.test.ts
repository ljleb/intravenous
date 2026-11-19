import assert from "node:assert/strict";
import test from "node:test";
import { sampleWindow, sampleWaveform } from "../src/sampleWaveform";
import { ObservationValues, type ObservationWindowResult, type ObservationUpdate } from "../src/observations";

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
    values.acceptGeneration(7);
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const data = result();
    values.expectWindows([{ id: "wave", source, operation: "waveform", window: data.window }]);
    const update = (window: ObservationWindowResult): ObservationUpdate => ({
        activationId: "0", generation: "7", revision: "0", index: "0", blockSize: 0, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    });
    assert.equal(values.receive(update(data), "viewport"), true);
    assert.equal(values.windows.get("wave")?.dataVersion, "4");
    values.receive(update({ ...data, dataVersion: "3" }), "viewport");
    assert.equal(values.windows.get("wave")?.dataVersion, "4");
    const next = { ...data.window, begin: "100" };
    values.expectWindows([{ id: "wave", source, operation: "waveform", window: next }]);
    assert.equal(values.windows.size, 0);
    values.receive(update(data), "viewport");
    assert.equal(values.windows.size, 0);
    values.receive(update({ ...data, window: next, regions: [] }), "viewport");
    assert.equal(values.windows.size, 1);
    values.acceptGeneration(8);
    values.receive(update({ ...data, window: next }), "viewport");
    assert.equal(values.windows.size, 0);
});
test("window previews reproject the last ready result while an overlapping viewport loads", () => {
    const values = new ObservationValues();
    values.acceptGeneration(7);
    const data = result();
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const request = { id: "wave", source, operation: "waveform", window: data.window };
    const receive = (window: ObservationWindowResult) => values.receive({
        activationId: "0", generation: "7", revision: "0", index: "0", blockSize: 0, dataVersion: "4",
        scopes: [{ scopeId: "viewport", values: [], windows: [window] }],
    }, "viewport");
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
    values.acceptGeneration(8);
    assert.equal(values.windowForDisplay("wave"), undefined);
});

test("a ready window overlaps a new request across the signed timeline seam", () => {
    const values = new ObservationValues();
    const seam = 1n << 63n;
    const data = { ...result(), window: { begin: String(seam - 8n), length: "32", points: 16 } };
    const source = { node: { instanceId: "curve" }, kind: "output" as const, member: "value" };
    const request = { id: "wave", source, operation: "waveform", window: data.window };
    values.expectWindows([request]);
    values.receive({ activationId: "0", generation: "7", revision: "0", index: "0", blockSize: 0, dataVersion: "4",
        scopes: [{ scopeId: "viewport", values: [], windows: [data] }] }, "viewport");
    const next = { ...data.window, begin: String(5n - seam) };
    values.expectWindows([{ ...request, window: next }]);
    assert.equal(values.windowForDisplay("wave")?.outdated, true);
});

test("bound windows arriving before the matching graph scope are used when it arrives", () => {
    const values = new ObservationValues();
    values.acceptGeneration(7);
    const data = result();
    const bounds = ["min", "max"] as const;
    values.expectWindows(bounds.map((id) => ({ id, window: data.window, operation: "waveform",
        source: { node: { instanceId: "curve" }, kind: "input", member: id } })));
    const update: ObservationUpdate = {
        activationId: "1", generation: "8", revision: "0", index: "0", blockSize: 0, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [], windows: bounds.map((id) => ({ ...data, id })) }],
    };
    assert.equal(values.receive(update, "viewport"), false);
    assert.equal(values.windows.size, 0);
    values.acceptGeneration(8);
    for (const id of bounds) assert.deepEqual(values.windows.get(id)?.regions, data.regions);
});
test("viewport Tock interests carry windows and panning never requests live observation operations", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
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
        scope.node(owner, node, true, window);
        await new Promise((resolve) => setTimeout(resolve, 250));
        const first = requests.at(-1);
        assert.ok(first?.type === "observationReconcile");
        assert.equal(first.interests.length, 1);
        assert.equal(first.interests[0].operation, "waveform");
        assert.deepEqual(first.interests[0].window, window);
        scope.node(owner, node, true, { ...window, begin: "1000" });
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

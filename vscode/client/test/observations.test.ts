import assert from "node:assert/strict";
import test from "node:test";
import { reactive } from "vue";
import { ObservationValues, observationSourceKey, type ObservationUpdate } from "../src/observations";

function update(activation: string, revision: string, available = true): ObservationUpdate {
    return { activationId: activation, revision, generation: "7", index: "-3", blockSize: 64, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [{ id: "out", available, value: { peak: .5 } }] }] };
}
test("observations reject obsolete activations and requirement revisions", () => {
    const values = new ObservationValues();
    values.acceptRevision("3");
    assert.equal(values.receive(update("12", "3"), "viewport"), true);
    assert.equal(values.receive(update("11", "3", false), "viewport"), false);
    values.acceptRevision("4");
    assert.equal(values.receive(update("13", "3", false), "viewport"), false);
    assert.equal(values.values.get("out")?.available, true);
    assert.equal(values.receive(update("14", "4", false), "viewport"), true);
    assert.equal(values.values.get("out")?.available, false);
    assert.equal(values.values.get("out")?.activationId, "14");
    // Another scope can change the aggregate requirement revision without
    // requiring this viewport to resubscribe or wait for its heartbeat.
    assert.equal(values.receive(update("15", "5"), "viewport"), true);
});
test("observations use exact activation IDs and ignore other scopes", () => {
    const values = new ObservationValues();
    values.acceptRevision("1");
    assert.equal(values.receive(update("9007199254740993", "1"), "viewport"), true);
    assert.equal(values.receive(update("9007199254740992", "1"), "viewport"), false);
    assert.equal(values.receive(update("9007199254740994", "1"), "other"), false);
    values.clear();
    assert.equal(values.values.size, 0);
});
test("observation source identity distinguishes channels, inputs, outputs, and state", () => {
    const source = { node: { instanceId: "n", path: [] }, kind: "output" as const, member: "value" };
    assert.notEqual(observationSourceKey(source), observationSourceKey({ ...source, channel: 0 }));
    assert.notEqual(observationSourceKey(source), observationSourceKey({ ...source, kind: "stateField" }));
    assert.notEqual(observationSourceKey(source), observationSourceKey({ ...source, kind: "input" }));
});

test("input observations use explicit interests alongside automatic output meters", { timeout: 5000 }, async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    type Reconcile = Extract<import("../src/observations").ObservationHostRequest, { type: "observationReconcile" }>;
    let publish: (request: Reconcile) => void;
    const scope = new PresentationObservations((request) => {
        const channels = [{ index: 0, name: "center" }];
        const nodeIndex = 0;
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [
                { nodeIndex, kind: "input", member: "gain", operations: ["value"], channels },
                { nodeIndex, kind: "input", member: "gain", delivery: "window", operations: ["waveform"], channels },
                { nodeIndex, kind: "output", member: "audio", operations: ["level"], channels },
            ] } : { revision: "1" } }));
        if (request.type === "observationReconcile") publish(request);
    });
    const nextRequest = () => new Promise<Reconcile>((resolve) => { publish = resolve; });
    const node = { instanceId: "n" };
    const source = { kind: "input" as const, node, member: "gain" };
    const live = scope.portObservationId(), range = scope.portObservationId(), prefetch = scope.portObservationId();
    const window = { begin: "0", length: "64", points: 8 };
    try {
        const installed = nextRequest();
        scope.node(Symbol("lane"), node, true);
        scope.observePort(prefetch, { source, operation: "waveform", window: { ...window, begin: "64" }, prefetch: true });
        scope.observePort(live, { source, operation: "value" });
        scope.observePort(range, { source, operation: "waveform", window });
        const interests = (await installed).interests;
        assert.deepEqual(interests.slice(0, 2), [
            { id: live, source, operation: "value" },
            { id: range, source, operation: "waveform", window },
        ]);
        assert.equal(interests[2].source.kind, "output");
        assert.equal(interests[3].id, prefetch);
        assert.equal(interests[3].prefetch, true);

        const removed = nextRequest();
        scope.observePort(live, undefined);
        scope.observePort(range, undefined);
        scope.observePort(prefetch, undefined);
        assert.deepEqual((await removed).interests.map((interest) => interest.source.kind), ["output"]);
    } finally { scope.dispose(); }
});

test("moving the viewport submits window updates before the gesture stops", async (context) => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    type Request = import("../src/observations").ObservationHostRequest;
    type Reconcile = Extract<Request, { type: "observationReconcile" }>;
    context.mock.timers.enable({ apis: ["setTimeout"] });
    const reconciles: Reconcile[] = [];
    let descriptions = 0;
    let revision = 0;
    const scope = new PresentationObservations((request) => {
        if (request.type === "observationDescribe") ++descriptions;
        else reconciles.push(request);
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [] } : { revision: String(++revision) } }));
    });
    const node = { instanceId: "curve" };
    const source = { node, kind: "input" as const, member: "min" };
    const id = scope.portObservationId();
    const owner = Symbol("lane");
    const windowAt = (begin: number) => ({ begin: String(begin), length: "64", points: 8 });
    const move = (begin: number) => {
        const window = windowAt(begin);
        scope.node(owner, node, true, window);
        scope.observePort(id, { source, operation: "waveform", window });
    };
    const tick = async (milliseconds: number) => {
        context.mock.timers.tick(milliseconds);
        await new Promise<void>((resolve) => setImmediate(resolve));
    };
    try {
        move(0);
        await tick(200);
        for (let position = 1; position <= 6; ++position) {
            move(position);
            await tick(20);
        }
        assert.equal(descriptions, 1);
        assert.equal(reconciles.length, 2);
        assert.deepEqual(reconciles[1].interests[0].window, windowAt(5));

        // The current gesture has already moved on, but its most recent
        // submitted result is still usable until we submit another window.
        const publish = (begin: number) => scope.receive({ type: "observationUpdate", update: {
            activationId: "1", generation: "1", revision: String(revision), index: "0", blockSize: 64, dataVersion: "0",
            scopes: [{ scopeId: "viewport", values: [], windows: [{ id, window: windowAt(begin),
                status: "ready", approximate: true, regions: [] }] }],
        } });
        publish(5);
        assert.deepEqual(scope.windowForDisplay(id)?.window, windowAt(5));
        await tick(100);
        assert.deepEqual(reconciles.at(-1)?.interests[0].window, windowAt(6));
        publish(5);
        assert.equal(scope.windowForDisplay(id)?.outdated, true);
        assert.deepEqual(scope.windowForDisplay(id)?.window, windowAt(5));
        publish(6);
        assert.deepEqual(scope.windowForDisplay(id)?.window, windowAt(6));
    } finally { scope.dispose(); }
});

test("default lane observations subscribe only to the requested first input, with its channels intact", { timeout: 5000 }, async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    type Reconcile = Extract<import("../src/observations").ObservationHostRequest, { type: "observationReconcile" }>;
    let publish: (request: Reconcile) => void;
    const stereo = [{ index: 0, name: "left" }, { index: 1, name: "right" }];
    const mono = [{ index: 0, name: "center" }];
    const scope = new PresentationObservations((request) => {
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [
                { nodeIndex: 0, kind: "input", member: "a-second", operations: ["level"], channels: mono },
                { nodeIndex: 0, kind: "input", member: "z-first", operations: ["level"], channels: stereo },
                { nodeIndex: 0, kind: "input", member: "z-first", delivery: "window", operations: ["waveform"], channels: stereo },
                { nodeIndex: 1, kind: "input", member: "z-first", operations: ["level"], channels: mono },
                { nodeIndex: 1, kind: "output", member: "value", operations: ["level"], channels: mono },
            ] } : { revision: "1" } }));
        if (request.type === "observationReconcile") publish(request);
    });
    const nextRequest = () => new Promise<Reconcile>((resolve) => { publish = resolve; });
    const sink = { instanceId: "sink" }, source = { instanceId: "source" };
    const owner = Symbol("sink");
    const window = { begin: "0", length: "64", points: 8 };
    try {
        const installed = nextRequest();
        scope.node(owner, sink, true, window, "z-first");
        scope.node(Symbol("source"), source, true, window);
        const { interests } = await installed;
        assert.deepEqual(interests.map(({ source, operation, window }) =>
            [source.node.instanceId, source.kind, source.member, source.channel, operation, window]), [
            ["sink", "input", "z-first", 0, "level", undefined],
            ["sink", "input", "z-first", 1, "level", undefined],
            ["sink", "input", "z-first", 0, "waveform", window],
            ["sink", "input", "z-first", 1, "waveform", window],
            ["source", "output", "value", 0, "level", undefined],
        ]);

        const changed = nextRequest();
        scope.node(owner, sink, true, window);
        assert.deepEqual((await changed).interests, [interests[4]]);
    } finally { scope.dispose(); }
});

test("explicit viewport windows coalesce and send before capability discovery", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    type Request = import("../src/observations").ObservationHostRequest;
    let firstRequest!: (request: Request) => void;
    const first = new Promise<Request>((resolve) => { firstRequest = resolve; });
    const scope = new PresentationObservations((request) => {
        firstRequest(request);
        scope.receive({ type: "observationResponse", requestId: request.requestId, result: { revision: "1" } });
    });
    const source = { node: { instanceId: "timeline" }, kind: "input" as const, portKind: "event" as const, member: "beats" };
    const window = { begin: "0", length: "1000", points: 128, maxDepth: 255 };
    try {
        scope.node(Symbol("meter"), { instanceId: "meter" }, true);
        scope.observePort("beats", { source, operation: "beat-grid", window }, "viewport");
        const latest = { ...window, begin: "256" };
        scope.observePort("beats", { source, operation: "beat-grid", window: latest }, "viewport");
        const request = await first;
        assert.equal(request.type, "observationReconcile");
        assert.deepEqual(request.interests, [{ id: "beats", source, operation: "beat-grid", window: latest }]);
    } finally { scope.dispose(); }
});

test("viewport observations clone reactive addresses and survive presentation changes", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    const requests: import("../src/observations").ObservationHostRequest[] = [];
    let revision = 0;
    const scope = new PresentationObservations((request) => {
        // Match the browser boundary: Vue proxies cannot be structured-cloned.
        requests.push(structuredClone(request));
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe"
                ? { capabilities: [{ nodeIndex: 0, kind: "output", member: "out", operations: ["level", "value"], channels: [{ index: 0, name: "center" }] }] }
                : { revision: String(++revision) } }));
    });
    const first = Symbol("first"), second = Symbol("second");
    const node = reactive({ instanceId: "n", path: [{ kind: "tiledChild", name: "oscillator" }] });
    const settle = () => new Promise((resolve) => setTimeout(resolve, 250));
    try {
        scope.node(first, node, true);
        scope.node(second, node, true);
        await settle();
        assert.equal(requests.filter((request) => request.type === "observationDescribe").length, 1);
        const firstReconcile = requests.find((request) => request.type === "observationReconcile");
        assert.equal(firstReconcile?.type === "observationReconcile" && firstReconcile.interests.length, 1);
        assert.equal(scope.error.value, "");
        assert.equal(firstReconcile?.type === "observationReconcile" && firstReconcile.interests[0].source.channel, 0);
        assert.deepEqual(firstReconcile?.type === "observationReconcile" && firstReconcile.interests[0].source.node, {
            instanceId: "n", path: [{ kind: "tiledChild", name: "oscillator" }],
        });
        scope.remove(first);
        await settle();
        const afterRemoval = requests.at(-1);
        assert.equal(afterRemoval?.type === "observationReconcile" && afterRemoval.interests.length, 1);
        scope.setVisible(false);
        await settle();
        const hidden = requests.at(-1);
        assert.deepEqual(hidden?.type === "observationReconcile" && hidden.interests, []);
        scope.setVisible(true);
        await settle();
        const restored = requests.at(-1);
        assert.equal(restored?.type === "observationReconcile" && restored.interests[0].id,
            firstReconcile?.type === "observationReconcile" && firstReconcile.interests[0].id);
    } finally { scope.dispose(); }
});


test("stereo meters keep their capture interests and readings across visual scale changes", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    const requests: import("../src/observations").ObservationHostRequest[] = [];
    let revision = 0;
    const scope = new PresentationObservations((request) => {
        requests.push(structuredClone(request));
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [{ nodeIndex: 0, kind: "output", member: "audio",
                operations: ["level", "value"], channels: [{ index: 0, name: "left" }, { index: 1, name: "right" }] }] }
                : { revision: String(++revision) } }));
    });
    const node = { instanceId: "stereo", path: [] };
    const source = { node, kind: "output" as const, member: "audio" };
    const owner = Symbol("meter");
    const settle = () => new Promise((resolve) => setTimeout(resolve, 250));
    try {
        scope.node(owner, node, true);
        await settle();
        const initial = requests.at(-1);
        assert.ok(initial?.type === "observationReconcile");
        assert.deepEqual(initial.interests.map((entry) => [entry.source.channel, entry.operation]), [[0, "level"], [1, "level"]]);
        assert.notEqual(initial.interests[0].id, initial.interests[1].id);
        assert.deepEqual(scope.capabilities.value.map((entry) => entry.channelName), ["left", "right"]);
        scope.receive({ type: "observationUpdate", update: { activationId: "1", generation: "1", revision: String(revision), index: "0", blockSize: 64, dataVersion: "0",
            scopes: [{ scopeId: "viewport", values: initial.interests.map((entry) => ({ id: entry.id, available: true,
                value: { peak: entry.source.channel === 0 ? .5 : .25, rms: .1, samples: "64", nonfinite: "0" } })) }] } });
        assert.equal(scope.values.values.size, 2);
        const beforeSwitch = requests.length;
        scope.setMode(source, "value");
        assert.equal(scope.values.values.size, 2);
        assert.equal(scope.mode(source), "value");
        await settle();
        assert.equal(requests.length, beforeSwitch); // No RPC or graph compilation for a scale change.
        assert.ok(scope.capabilities.value.every((entry) => entry.operation === "level"));
        const value = initial;
        scope.remove(owner);
        await settle();
        scope.node(owner, node, true);
        await settle();
        const restored = requests.at(-1);
        assert.ok(restored?.type === "observationReconcile");
        assert.deepEqual(restored.interests, value.interests);
        const beforeBack = requests.length;
        scope.setMode(source, "level");
        await settle();
        assert.equal(requests.length, beforeBack);
        assert.equal(scope.mode(source), "level");
        assert.deepEqual(restored.interests, initial.interests);
    } finally { scope.dispose(); }
});


test("compatible meter schemas retain identities across graph revisions while incompatible schemas clear old readings", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    let fieldType: "float32" | "float64" = "float64";
    let rightName = "right";
    const scope = new PresentationObservations((request) => {
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [{ nodeIndex: 0, kind: "output", member: "audio",
                operations: ["level"], channels: [{ index: 0, name: "left" }, { index: 1, name: rightName }],
                resultSchemas: [{ operation: "level", fields: [{ name: "peak", type: fieldType }] }] }] }
                : { revision: "1" } }));
    });
    const owner = Symbol("meter"), node = { instanceId: "stereo", path: [] };
    const settle = () => new Promise((resolve) => setTimeout(resolve, 250));
    try {
        scope.configuredGeneration(1);
        scope.node(owner, node, true);
        await settle();
        const initial = scope.capabilities.value.map((entry) => entry.id);
        assert.equal(initial.length, 2);
        scope.configuredGeneration(2);
        scope.node(owner, structuredClone(node), true);
        scope.refresh();
        await settle();
        assert.deepEqual(scope.capabilities.value.map((entry) => entry.id), initial);
        scope.receive({ type: "observationUpdate", update: { generation: "2", activationId: "2", revision: "1", dataVersion: "0",
            index: "0", blockSize: 64, scopes: [{ scopeId: "viewport", values: initial.map((id) => ({
                id, available: true, value: { peak: .5, rms: .1, samples: "64", nonfinite: "0" },
            })) }] } });
        assert.equal(scope.values.values.size, 2);

        // A neighbouring channel's changed contract cannot reset the unchanged
        // left channel, but the changed right channel must lose its old data.
        rightName = "changed right";
        scope.refresh();
        await settle();
        assert.equal(scope.capabilities.value[0].id, initial[0]);
        assert.notEqual(scope.capabilities.value[1].id, initial[1]);
        assert.equal(scope.values.values.has(initial[0]), true);
        assert.equal(scope.values.values.has(initial[1]), false);

        fieldType = "float32";
        scope.refresh();
        await settle();
        assert.notEqual(scope.capabilities.value[0].id, initial[0]);
        assert.equal(scope.values.values.size, 0);
    } finally { scope.dispose(); }
});

test("viewport probe budget keeps complete stereo pairs", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    const requests: import("../src/observations").ObservationHostRequest[] = [];
    const scope = new PresentationObservations((request) => {
        requests.push(request);
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [
                ...Array.from({ length: 63 }, (_, index) => ({ nodeIndex: 0, kind: "output", member: `mono${index}`,
                    operations: ["level", "value"], channels: [{ index: 0, name: "center" }] })),
                { nodeIndex: 0, kind: "output", member: "stereo", operations: ["level", "value"],
                    channels: [{ index: 0, name: "left" }, { index: 1, name: "right" }] },
            ] } : { revision: "1" } }));
    });
    try {
        scope.node(Symbol(), { instanceId: "many-outputs" }, true);
        await new Promise((resolve) => setTimeout(resolve, 250));
        const request = requests.at(-1);
        assert.ok(request?.type === "observationReconcile");
        assert.equal(request.interests.length, 63);
        assert.ok(request.interests.every((entry) => entry.source.member !== "stereo"));
    } finally { scope.dispose(); }
});

test("meter display choices persist as frontend preferences without project mutations", () => {
    const source = { node: { instanceId: "node", path: [] }, kind: "output" as const, member: "out" };
    const key = observationSourceKey(source);
    let saved: Record<string, import("../src/observations").MeterMode> = {};
    // Construction/disposal do not issue RPC requests without visible interests.
    return import("../webview/presentation/observationContext").then(({ PresentationObservations }) => {
        const scope = new PresentationObservations(() => assert.fail("no visible sources"), {
            initial: { [key]: "value" }, save: (modes) => { saved = modes; },
        });
        const input = { ...source, kind: "input" as const };
        assert.equal(scope.mode(source), "value");
        assert.equal(scope.mode(input), "level");
        scope.setMode(input, "value");
        scope.setMode(source, "level");
        assert.equal(saved[key], "level");
        scope.dispose();
        const restored = new PresentationObservations(() => assert.fail("no visible sources"), {
            initial: saved, save: () => {},
        });
        assert.equal(restored.mode(source), "level");
        assert.equal(restored.mode(input), "value");
        restored.dispose();
    });
});


test("configured generation changes clear measurements and reject delayed older executable frames", () => {
    const values = new ObservationValues();
    const update = (generation: string): ObservationUpdate => ({
        activationId: "1", generation, revision: "1", index: "0", blockSize: 64, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [{ id: "peak", available: true, value: { peak: .5 } }] }],
    });
    values.acceptGeneration(4);
    assert.equal(values.receive(update("4"), "viewport"), true);
    assert.equal(values.values.get("peak")?.available, true);
    assert.equal(values.acceptGeneration(5), true);
    assert.equal(values.values.size, 0);
    assert.equal(values.receive(update("4"), "viewport"), false);
    assert.equal(values.receive(update("5"), "viewport"), true);
    assert.equal(values.acceptGeneration(5), false);
    assert.equal(values.values.get("peak")?.available, true);
    values.acceptGeneration(null);
    assert.equal(values.receive(update("5"), "viewport"), false);
    values.clear();
    assert.equal(values.values.size, 0);
    values.acceptGeneration(4);
    assert.equal(values.receive(update("4"), "viewport"), true);
});

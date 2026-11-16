import assert from "node:assert/strict";
import test from "node:test";
import { reactive } from "vue";
import { ObservationValues, observationSourceKey, type ObservationUpdate } from "../src/observations";

function update(activation: string, revision: string, available = true): ObservationUpdate {
    return { activationId: activation, revision, generation: "7", index: "-3", blockSize: 64,
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
test("observation source identity includes channels and state versus output", () => {
    const source = { node: { instanceId: "n", path: [] }, kind: "output" as const, member: "value" };
    assert.notEqual(observationSourceKey(source), observationSourceKey({ ...source, channel: 0 }));
    assert.notEqual(observationSourceKey(source), observationSourceKey({ ...source, kind: "stateField" }));
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
        scope.receive({ type: "observationUpdate", update: { activationId: "1", generation: "1", revision: String(revision), index: "0", blockSize: 64,
            scopes: [{ scopeId: "viewport", values: initial.interests.map((entry) => ({ id: entry.id, available: true,
                value: { peak: entry.source.channel === 0 ? .5 : .25, rms: .1, samples: "64", nonfinite: "0" } })) }] } });
        assert.equal(scope.values.values.size, 2);
        const beforeSwitch = requests.length;
        scope.setMode(node, "audio", "value");
        assert.equal(scope.values.values.size, 2);
        assert.equal(scope.mode(node, "audio"), "value");
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
        scope.setMode(node, "audio", "level");
        await settle();
        assert.equal(requests.length, beforeBack);
        assert.equal(scope.mode(node, "audio"), "level");
        assert.deepEqual(restored.interests, initial.interests);
    } finally { scope.dispose(); }
});


test("viewport probe budget keeps complete stereo pairs and status ownership clears on disconnect", async () => {
    const { PresentationObservations } = await import("../webview/presentation/observationContext");
    const requests: import("../src/observations").ObservationHostRequest[] = [];
    const hints: Array<import("../src/observations").ObservationHint | null> = [];
    const scope = new PresentationObservations((request) => {
        requests.push(request);
        queueMicrotask(() => scope.receive({ type: "observationResponse", requestId: request.requestId,
            result: request.type === "observationDescribe" ? { capabilities: [
                ...Array.from({ length: 63 }, (_, index) => ({ nodeIndex: 0, kind: "output", member: `mono${index}`,
                    operations: ["level", "value"], channels: [{ index: 0, name: "center" }] })),
                { nodeIndex: 0, kind: "output", member: "stereo", operations: ["level", "value"],
                    channels: [{ index: 0, name: "left" }, { index: 1, name: "right" }] },
            ] } : { revision: "1" } }));
    }, (hint) => hints.push(hint));
    try {
        scope.node(Symbol(), { instanceId: "many-outputs" }, true);
        await new Promise((resolve) => setTimeout(resolve, 250));
        const request = requests.at(-1);
        assert.ok(request?.type === "observationReconcile");
        assert.equal(request.interests.length, 63);
        assert.ok(request.interests.every((entry) => entry.source.member !== "stereo"));
        const first = Symbol(), second = Symbol();
        scope.hint(first, { text: "left", tooltip: "left" });
        scope.hint(second, { text: "right", tooltip: "right" });
        scope.hint(second, null);
        assert.equal(hints.at(-1)?.text, "left");
        scope.disconnect();
        assert.equal(hints.at(-1), null);
    } finally { scope.dispose(); }
});


test("meter display choices persist as frontend preferences without project mutations", () => {
    const source = { node: { instanceId: "node", path: [] }, kind: "output" as const, member: "out" };
    const key = observationSourceKey(source);
    let saved: Record<string, import("../src/observations").MeterMode> = {};
    // Construction/disposal do not issue RPC requests without visible interests.
    return import("../webview/presentation/observationContext").then(({ PresentationObservations }) => {
        const scope = new PresentationObservations(() => assert.fail("no visible sources"), () => {}, {
            initial: { [key]: "value" }, save: (modes) => { saved = modes; },
        });
        assert.equal(scope.mode(source.node, source.member), "value");
        scope.setMode(source.node, source.member, "level");
        assert.equal(saved[key], "level");
        scope.dispose();
        const restored = new PresentationObservations(() => assert.fail("no visible sources"), () => {}, {
            initial: saved, save: () => {},
        });
        assert.equal(restored.mode(source.node, source.member), "level");
        restored.dispose();
    });
});

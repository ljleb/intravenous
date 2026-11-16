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
                ? { capabilities: [{ nodeIndex: 0, kind: "output", member: "out", operations: ["level"] }] }
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

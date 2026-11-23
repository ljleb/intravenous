import test from "node:test";
import assert from "node:assert/strict";

import { JsonRpcSocketClient } from "../src/rpcClient";
import { WorkspaceRpc } from "../src/workspaceRpc";

test("workspace mutations include the current project session revision", async () => {
    const requests: Array<{
        method: string;
        params: Record<string, unknown>;
    }> = [];
    const client = {
        request: async (method: string, params: Record<string, unknown>) => {
            requests.push({ method, params });
            return undefined;
        },
    } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 17);

    await rpc.deleteNodeInstance("instance:one");

    assert.deepEqual(requests, [{
        method: "nodeInstances.delete",
        params: {
            instanceId: "instance:one",
            expectedRevision: 17,
        },
    }]);
});

test("graph scope reads use the existing session without a mutation revision", async () => {
    const calls: unknown[] = [];
    const scope = { generation: 4, editable: true, nodes: [], connections: [] };
    const client = { request: async (method, params) => { calls.push({ method, params }); return scope; } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 17);
    assert.deepEqual(await rpc.getGraphScope(), scope);
    const tile = { instanceId: "module", path: [{ kind: "tiledChild", name: "right" }] };
    assert.deepEqual(await rpc.getGraphScope([{ instanceId: "module" }, tile]), scope);
    assert.deepEqual(calls, [{ method: "graph.getScope", params: {} },
        { method: "graph.getScope", params: { expanded: [{ instanceId: "module", path: [] }, tile] } }]);
});

test("connection edits retain the revision captured before the scope read", async () => {
    let revision = 17;
    const calls: unknown[] = [];
    const client = { request: async (method, params) => {
        calls.push({ method, params });
        if (method === "graph.getScope") revision = 18; // Another view edited the project.
        return { generation: 4, editable: true, nodes: [], connections: [] };
    } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => revision);
    const beforeRead = revision;
    await rpc.getGraphScope();
    const connection = { connectionId: "wire", kind: "sample" };
    await rpc.upsertGraphConnection(connection, beforeRead);
    await rpc.deleteGraphConnection("wire", beforeRead);
    await rpc.upsertGraphConnection(connection); // Existing callers still use the current revision.
    assert.deepEqual(calls, [
        { method: "graph.getScope", params: {} },
        { method: "graphConnections.upsert", params: { connection, expectedRevision: 17 } },
        { method: "graphConnections.delete", params: { connectionId: "wire", expectedRevision: 17 } },
        { method: "graphConnections.upsert", params: { connection, expectedRevision: 18 } },
    ]);
});

test("project history and a multi-node edit use one revision-checked request each", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method, params) => { calls.push({ method, params }); } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 23);
    const operations = ["a", "b"].map((instanceId) => ({ method: "nodeInstances.delete", params: { instanceId } }));
    await rpc.applyProjectEdit(operations, 19);
    await rpc.projectHistory("undo");
    await rpc.projectHistory("redo");
    assert.deepEqual(calls, [
        { method: "project.applyEdit", params: { operations, expectedRevision: 19 } },
        { method: "project.undo", params: { expectedRevision: 23 } },
        { method: "project.redo", params: { expectedRevision: 23 } },
    ]);
});

test("lane view patches and save on blur do not require a graph revision", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method, params) => { calls.push({ method, params }); } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 23);
    const operations = [{ method: "projectPresentation.setLaneView",
        params: { address: { instanceId: "curve", path: [] }, presentationId: "curve", state: '{"pan":-4}' } }];
    await rpc.applyProjectEdit(operations, null);
    await rpc.saveProject(true);
    await rpc.saveProject();
    assert.deepEqual(calls, [
        { method: "project.applyEdit", params: { operations } },
        { method: "project.save", params: { autosaveOnly: true } },
        { method: "project.save", params: {} },
    ]);
});

test("manual control gestures survive revision changes while standalone edits check their revision", async () => {
    const calls: unknown[] = [];
    let revision = 1;
    const client = {
        request: async (method, params) => { calls.push({ method, params }); ++revision; },
        notify: (method, params) => { calls.push({ notification: method, params }); },
    } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => revision);
    const control = { node: { instanceId: "filter", path: [{ name: "inner" }] }, inputName: "cutoff" };
    await rpc.hoverManualControl(control);
    await rpc.beginManualControl("drag", control, .2);
    rpc.updateManualControl("drag", .3);
    await rpc.endManualControl("drag", .4);
    await rpc.hoverManualControl(null);
    await rpc.setManualInputEnabled(control, .4, false, "disconnectedOnly", 7);
    assert.deepEqual(calls, [
        { method: "manualControls.hover", params: control },
        { method: "manualControls.begin", params: { interactionId: "drag", ...control, value: .2 } },
        { notification: "manualControls.update", params: { interactionId: "drag", value: .3 } },
        { method: "manualControls.end", params: { interactionId: "drag", value: .4 } },
        { method: "manualControls.hover", params: {} },
        { method: "manualControls.upsert", params: { ...control, value: .4, enabled: false, participation: "disconnectedOnly", expectedRevision: 7 } },
    ]);
});

test("audio output diagnostics read the existing audioDevices endpoint without a project mutation", async () => {
    const calls: unknown[] = [];
    const snapshot = { selectedOutput: { deviceId: "default", available: false, error: "unavailable" } };
    const client = { request: async (method, params) => { calls.push({ method, params }); return snapshot; } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 17);
    assert.deepEqual(await rpc.getAudioDevices(), snapshot);
    assert.deepEqual(calls, [{ method: "audioDevices.get", params: {} }]);
});

test("typed node messages pin the revision before reading receiver support and retain exact event identity", async () => {
    const { MovePoint } = await import("../src/generated/automationEvents");
    const { serializeNodeEvent } = await import("../src/nodeEvents");
    const event = serializeNodeEvent(MovePoint({ id: 4, seconds: 0, value: .5, sample_index: "96000" }));
    const calls: unknown[] = [];
    let revision = 17;
    const client = { request: async (method, params) => {
        calls.push({ method, params });
        if (method === "nodeMessages.getProtocol") revision = 18;
        return { ok: true, diagnostics: [] };
    } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => revision);
    const beforeRead = revision;
    await rpc.getNodeEventProtocol();
    await rpc.dispatchNodeEvent({ instanceId: "curve" }, event, beforeRead);
    assert.deepEqual(calls, [
        { method: "nodeMessages.getProtocol", params: {} },
        { method: "nodeMessages.dispatch", params: {
            target: { instanceId: "curve", path: [] },
            eventTypeNominalId: event.eventType.nominalId,
            eventTypeDefinitionFingerprint: event.eventType.definitionFingerprint,
            payload: { id: 4, seconds: 0, value: .5, sample_index: "96000" }, expectedRevision: 17,
        } },
    ]);
});

test("open node gestures use their target and interaction ID across project revisions", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method, params) => { calls.push({ method, params }); } } as unknown as JsonRpcSocketClient;
    let revision = 23;
    const rpc = new WorkspaceRpc(client, () => revision);
    for (const target of [
        { instanceId: "curve" },
        { instanceId: "curve", path: [] },
        { instanceId: "curve", path: [{ kind: "symbolicNode", sourceIdentity: "inner" }] },
    ]) {
        calls.length = 0;
        const original = structuredClone(target);
        revision = 23;
        await rpc.nodeEventGesture(target, "drag", true);
        revision = 24;
        await rpc.dispatchNodeEvent(target, { eventType: { nominalId: "point", definitionFingerprint: "v0" }, payload: {} }, 24, "drag");
        revision = 25;
        await rpc.nodeEventGesture(target, "drag", false);
        const wireTarget = { ...target, path: target.path ?? [] };
        assert.deepEqual(calls, [
            { method: "nodeMessages.beginGesture", params: { target: wireTarget, interactionId: "drag", expectedRevision: 23 } },
            { method: "nodeMessages.dispatch", params: { target: wireTarget, eventTypeNominalId: "point", eventTypeDefinitionFingerprint: "v0",
                payload: {}, interactionId: "drag" } },
            { method: "nodeMessages.endGesture", params: { target: wireTarget, interactionId: "drag" } },
        ]);
        assert.deepEqual(target, original);
    }
});


test("observation reads normalize root node addresses without adding a mutation revision", async () => {
    const requests: unknown[] = [];
    const client = { request: async (method, params) => { requests.push({ method, params }); } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 23);
    const node = { instanceId: "oscillator" };
    await rpc.describeObservations([node]);
    await rpc.reconcileObservations("viewport", [{ id: "level", source: { node, kind: "output", member: "out" }, operation: "level" }]);
    assert.deepEqual(requests, [
        { method: "observations.describe", params: { nodes: [{ instanceId: "oscillator", path: [] }] } },
        { method: "observations.reconcile", params: { scopeId: "viewport", interests: [
            { id: "level", source: { node: { instanceId: "oscillator", path: [] }, kind: "output", member: "out" }, operation: "level" },
        ] } },
    ]);
    assert.deepEqual(node, { instanceId: "oscillator" });
});


test("output gain requests use outputName without inventing an input identity", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method, params) => { calls.push({ method, params }); } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => 9);
    const gain = { node: { instanceId: "synth" }, outputName: "main" };
    await rpc.hoverManualControl(gain);
    await rpc.beginManualControl("gain-drag", gain, 1);
    assert.deepEqual(calls, [
        { method: "manualControls.hover", params: gain },
        { method: "manualControls.begin", params: { interactionId: "gain-drag", ...gain, value: 1 } },
    ]);
});

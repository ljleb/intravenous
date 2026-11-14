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
    assert.deepEqual(calls, [{ method: "graph.getScope", params: {} }]);
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

test("manual control requests retain server client identity and use fresh mutation revisions", async () => {
    const calls: unknown[] = [];
    let revision = 1;
    const client = { request: async (method, params) => { calls.push({ method, params }); ++revision; } } as unknown as JsonRpcSocketClient;
    const rpc = new WorkspaceRpc(client, () => revision);
    const control = { node: { instanceId: "filter", path: [{ name: "inner" }] }, inputName: "cutoff" };
    await rpc.hoverManualControl(control);
    await rpc.beginManualControl("drag", control, .2);
    await rpc.updateManualControl("drag", .3);
    await rpc.endManualControl("drag", .4);
    await rpc.hoverManualControl(null);
    assert.deepEqual(calls, [
        { method: "manualControls.hover", params: control },
        { method: "manualControls.begin", params: { interactionId: "drag", ...control, value: .2, expectedRevision: 2 } },
        { method: "manualControls.update", params: { interactionId: "drag", value: .3, expectedRevision: 3 } },
        { method: "manualControls.end", params: { interactionId: "drag", value: .4, expectedRevision: 4 } },
        { method: "manualControls.hover", params: {} },
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

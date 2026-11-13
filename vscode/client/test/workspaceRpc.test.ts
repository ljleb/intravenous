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

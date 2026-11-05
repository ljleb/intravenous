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

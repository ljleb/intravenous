import test from "node:test";
import assert from "node:assert/strict";
import { NodeEventInteractions } from "../src/nodeEventInteractions";
import { serializeNodeEvent } from "../src/nodeEvents";
import { MovePoint } from "../src/generated/automationEvents";

const target = { instanceId: "curve" };
const event = serializeNodeEvent(MovePoint({ id: 1, seconds: 1, value: .5 }));
function deferred() {
    let resolve!: () => void;
    const promise = new Promise<void>((done) => { resolve = done; });
    return { promise, resolve };
}

test("a closed webview waits for its pending begin and updates before closing history", async () => {
    const opening = deferred(), ended = deferred();
    const calls: string[] = [];
    const owner = new NodeEventInteractions({
        begin: async () => { calls.push("begin"); await opening.promise; },
        send: async () => { calls.push("send"); return 4; },
        end: async () => { calls.push("end"); ended.resolve(); },
        report: (error) => assert.fail(String(error)),
    });
    const begin = owner.begin("view", target, "drag");
    const send = owner.send("view", target, event, "drag");
    owner.releaseOwner("view");
    await assert.rejects(owner.send("view", target, event, "drag"), /ended/);
    opening.resolve();
    await begin; await send; await ended.promise;
    assert.deepEqual(calls, ["begin", "send", "end"]);
});

test("another view cannot use a gesture and a failed update is not retried", async () => {
    const calls: string[] = [];
    const owner = new NodeEventInteractions({ begin: async () => {},
        send: async () => { calls.push("send"); throw new Error("gesture conflicted"); },
        end: async () => { calls.push("end"); }, report: () => {} });
    await owner.begin("view", target, "drag");
    await assert.rejects(owner.send("other", target, event, "drag"), /another view/);
    await assert.rejects(owner.send("view", target, event, "drag"), /conflicted/);
    await assert.rejects(owner.send("view", target, event, "drag"), /conflicted|failed/);
    await owner.end("view", target, "drag");
    assert.deepEqual(calls, ["send", "end"]);
});

test("disconnect prevents queued cleanup from being submitted to the next server", async () => {
    const opening = deferred();
    let ended = 0;
    const owner = new NodeEventInteractions({ begin: async () => opening.promise, send: async () => {},
        end: async () => { ++ended; }, report: () => {} });
    const begin = owner.begin("view", target, "drag");
    await Promise.resolve();
    const end = owner.end("view", target, "drag");
    owner.disconnected();
    opening.resolve();
    await begin; await end;
    assert.equal(ended, 0);
});

import test from "node:test";
import assert from "node:assert/strict";
import { MovePoint } from "../src/generated/automationEvents";
import { isNodeEventRequest, NodeEventMessaging, replyToNodeEvent, type NodeEventRequest } from "../src/nodeEventMessaging";
import type { NodeEvent } from "../src/nodeEvents";

const event = () => MovePoint({ id: 4, seconds: 2, value: .5 });

test("node event completions survive presentation replacement and correlate out-of-order replies", async () => {
    const requests: NodeEventRequest[] = [];
    let sequence = 0;
    const connection = new NodeEventMessaging((request) => { assert.ok(isNodeEventRequest(request)); requests.push(request); }, () => String(++sequence));
    const target = { instanceId: "curve" };
    // Presentation owners can disappear while this connection stays alive.
    let presentation: { send(event: NodeEvent): Promise<void> } | null = {
        send: (value) => connection.send(target, value),
    };
    const first = presentation.send(event());
    presentation = null;
    const second = connection.send(target, event());
    target.instanceId = "different";
    assert.equal(requests[0].target.instanceId, "curve");
    assert.equal(isNodeEventRequest(requests[0]), true);
    assert.equal(isNodeEventRequest({ type: "sendNodeEvent", requestId: "bad" }), false);
    const failure = assert.rejects(second, /receiver removed/);
    connection.receive(await replyToNodeEvent(requests[1], async () => { throw new Error("receiver removed"); }));
    connection.receive(await replyToNodeEvent(requests[0], async (address, sent) => {
        assert.equal(address.instanceId, "curve");
        assert.deepEqual(sent.payload, { id: 4, seconds: 2, value: .5 });
    }));
    await first; await failure;
});

test("disconnect rejects pending sends and late replies cannot complete a newer request", async () => {
    const requests: NodeEventRequest[] = [];
    let sequence = 0;
    const connection = new NodeEventMessaging((request) => { assert.ok(isNodeEventRequest(request)); requests.push(request); }, () => String(++sequence));
    const pending = assert.rejects(connection.send({ instanceId: "curve" }, event()), /connection closed/);
    connection.disconnected();
    await pending;
    let completed = false;
    const next = connection.send({ instanceId: "curve" }, event()).then(() => { completed = true; });
    connection.receive({ type: "nodeEventResult", requestId: requests[0].requestId });
    await Promise.resolve();
    assert.equal(completed, false);
    connection.receive({ type: "nodeEventResult", requestId: requests[1].requestId });
    await next;
    await assert.rejects(connection.send(undefined, event()), /stable node address/);
    await assert.rejects(connection.send({ instanceId: "curve" }, {} as NodeEvent), /typed event/);
});

test("a lazy node gesture sends begin, payload and end in order and retains the accepted revision", async () => {
    const { isNodeEventGestureRequest, replyToNodeEventGesture } = await import("../src/nodeEventMessaging");
    let sequence = 0;
    let connection: NodeEventMessaging;
    const calls: string[] = [];
    connection = new NodeEventMessaging((request) => {
        if (isNodeEventRequest(request)) {
            calls.push("send");
            assert.ok(request.interactionId);
            void replyToNodeEvent(request, async () => 8).then((response) => connection.receive(response));
        } else {
            assert.ok(isNodeEventGestureRequest(request));
            calls.push(request.begin ? "begin" : "end");
            void replyToNodeEventGesture(request, async () => request.begin ? 7 : 9)
                .then((response) => connection.receive(response));
        }
    }, () => String(++sequence));
    const gesture = connection.gesture({ instanceId: "curve" });
    assert.deepEqual(calls, []);
    await gesture.send(event());
    await gesture.finish();
    assert.deepEqual(calls, ["begin", "send", "end"]);
    // History closure takes revision 9 but changes no point data. A read at
    // the payload's revision 8 is already authoritative for its final value.
    assert.equal(gesture.acceptedRevision?.(), 8);
    await gesture.finish();
    assert.deepEqual(calls, ["begin", "send", "end"]);
    await assert.rejects(gesture.send(event()), /ended/);
    const old = connection.gesture({ instanceId: "curve" });
    connection.disconnected();
    await assert.rejects(old.send(event()), /connection closed/);
});

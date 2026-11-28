import type { NodeEventInteractions } from "./nodeEventInteractions";
import type { NodeAddress } from "./graphScope";
import { serializeNodeEvent, type NodeEvent, type SerializedNodeEvent } from "./nodeEvents";

export type NodeEventRequest = { type: "sendNodeEvent"; requestId: string; target: NodeAddress; event: SerializedNodeEvent; interactionId?: string };
export type NodeEventGestureRequest = { type: "nodeEventGesture"; requestId: string; target: NodeAddress; interactionId: string; begin: boolean };
export type NodeEventHostRequest = NodeEventRequest | NodeEventGestureRequest;
export type NodeEventGesture = { send(event: NodeEvent): Promise<void>; finish(): Promise<void>; acceptedRevision?(): number | undefined };
export type NodeEventResponse = { type: "nodeEventResult"; requestId: string; error?: string; acceptedRevision?: number };

export function isNodeEventRequest(value: unknown): value is NodeEventRequest {
    if (!value || typeof value !== "object") return false;
    const request = value as NodeEventRequest;
    return request.type === "sendNodeEvent" && typeof request.requestId === "string" && !!request.requestId
        && typeof request.target?.instanceId === "string" && !!request.target.instanceId
        && typeof request.event?.eventType?.nominalId === "string"
        && typeof request.event?.eventType?.definitionFingerprint === "string"
        && (request.interactionId === undefined || (typeof request.interactionId === "string" && !!request.interactionId))
        && Object.hasOwn(request.event, "payload");
}

export function isNodeEventGestureRequest(value: unknown): value is NodeEventGestureRequest {
    if (!value || typeof value !== "object") return false;
    const request = value as NodeEventGestureRequest;
    return request.type === "nodeEventGesture" && typeof request.requestId === "string" && !!request.requestId
        && typeof request.target?.instanceId === "string" && !!request.target.instanceId
        && typeof request.interactionId === "string" && !!request.interactionId && typeof request.begin === "boolean";
}

// Owned by a webview root, not by presentation components. A remount or mode
// change does not discard a reliable dispatch's completion or failure.
export class NodeEventMessaging {
    private readonly pending = new Map<string, { resolve(revision?: number): void; reject(error: Error): void }>();
    private epoch = 0;
    constructor(private readonly post: (request: NodeEventHostRequest) => void, private readonly newId: () => string) {}
    send(target: NodeAddress | undefined, event: NodeEvent, interactionId?: string): Promise<void> {
        if (!target) return Promise.reject(new Error("This presentation has no stable node address"));
        let serialized: SerializedNodeEvent;
        try { serialized = serializeNodeEvent(event); }
        catch (error) { return Promise.reject(error); }
        return this.sendRequest(target, serialized, interactionId).then(() => {});
    }
    private sendRequest(target: NodeAddress, event: SerializedNodeEvent, interactionId?: string): Promise<number | undefined> {
        return this.request({ type: "sendNodeEvent", requestId: this.newId(), target, event, interactionId });
    }
    gesture(target: NodeAddress | undefined): NodeEventGesture {
        if (!target) throw new Error("This presentation has no stable node address");
        const address = structuredClone(target);
        const interactionId = this.newId();
        const epoch = this.epoch;
        let started = false;
        let ended = false;
        let pending: Promise<void> = Promise.resolve();
        let acceptedRevision: number | undefined;
        return {
            send: (event) => {
                if (epoch !== this.epoch) return Promise.reject(new Error("The node-event connection closed"));
                if (ended) return Promise.reject(new Error("Node-event gesture ended"));
                if (!started) {
                    started = true;
                    pending = this.request({ type: "nodeEventGesture", requestId: this.newId(), target: address, interactionId, begin: true }).then(() => {});
                }
                pending = pending.then(() => this.sendRequest(address, serializeNodeEvent(event), interactionId)).then((revision) => { acceptedRevision = revision; });
                void pending.catch(() => {});
                return pending;
            },
            finish: () => {
                if (epoch !== this.epoch) return Promise.resolve();
                if (ended) return pending;
                ended = true;
                if (!started) return pending;
                pending = pending.catch(() => {}).then(() => this.request({ type: "nodeEventGesture", requestId: this.newId(),
                    target: address, interactionId, begin: false })).then(() => {});
                void pending.catch(() => {});
                return pending;
            },
            acceptedRevision: () => acceptedRevision,
        };
    }
    private request(message: NodeEventHostRequest): Promise<number | undefined> {
        const { requestId } = message;
        return new Promise((resolve, reject) => {
            this.pending.set(requestId, { resolve, reject });
            try { this.post(structuredClone(message)); }
            catch (error) { this.pending.delete(requestId); reject(error); }
        });
    }
    receive(message: NodeEventResponse): void {
        const request = this.pending.get(message.requestId);
        if (!request) return;
        this.pending.delete(message.requestId);
        if (message.error) request.reject(new Error(message.error));
        else request.resolve(message.acceptedRevision);
    }
    disconnected(): void {
        ++this.epoch;
        for (const request of this.pending.values()) request.reject(new Error("The node-event connection closed"));
        this.pending.clear();
    }
}

export async function replyToNodeEventGesture(request: NodeEventGestureRequest,
    dispatch: (target: NodeAddress, interactionId: string, begin: boolean) => Promise<number | void>): Promise<NodeEventResponse> {
    try {
        const revision = await dispatch(request.target, request.interactionId, request.begin);
        return { type: "nodeEventResult", requestId: request.requestId, ...(typeof revision === "number" ? { acceptedRevision: revision } : {}) };
    } catch (error) {
        return { type: "nodeEventResult", requestId: request.requestId,
            error: error instanceof Error ? error.message : String(error) };
    }
}

export async function replyToNodeEvent(request: NodeEventRequest,
    dispatch: (target: NodeAddress, event: SerializedNodeEvent, interactionId?: string) => Promise<number | void>): Promise<NodeEventResponse> {
    try {
        const revision = await dispatch(request.target, request.event, request.interactionId);
        return { type: "nodeEventResult", requestId: request.requestId, ...(typeof revision === "number" ? { acceptedRevision: revision } : {}) };
    } catch (error) {
        return { type: "nodeEventResult", requestId: request.requestId,
            error: error instanceof Error ? error.message : String(error) };
    }
}

export function answerNodeEventRequest(interactions: NodeEventInteractions, owner: string,
    request: NodeEventHostRequest): Promise<NodeEventResponse> {
    return request.type === "sendNodeEvent"
        ? replyToNodeEvent(request, (target, event, id) => interactions.send(owner, target, event, id))
        : replyToNodeEventGesture(request, (target, id, begin) => begin
            ? interactions.begin(owner, target, id) : interactions.end(owner, target, id));
}

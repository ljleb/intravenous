import { addressKey, type NodeAddress } from "./graphScope";
import type { SerializedNodeEvent } from "./nodeEvents";

type OpenGesture = { owner: string; target: NodeAddress; pending: Promise<number | void>; closed: boolean; failed: boolean };
type Environment = {
    begin(target: NodeAddress, interactionId: string): Promise<number | void>;
    send(target: NodeAddress, event: SerializedNodeEvent, interactionId?: string): Promise<number | void>;
    end(target: NodeAddress, interactionId: string): Promise<number | void>;
    report(error: unknown): void;
};

// The extension session owns these queues. A webview can disappear while its
// begin is still pending; cleanup waits for it, then closes accepted history.
export class NodeEventInteractions {
    private readonly gestures = new Map<string, OpenGesture>();
    private readonly closing = new Set<OpenGesture>();
    constructor(private readonly env: Environment) {}
    begin(owner: string, target: NodeAddress, id: string): Promise<number | void> {
        if (!id || this.gestures.has(id)) return Promise.reject(new Error("Node-event gesture is already open or has no id"));
        const open: OpenGesture = { owner, target: structuredClone(target), pending: Promise.resolve(), closed: false, failed: false };
        this.gestures.set(id, open);
        return this.queue(open, () => this.env.begin(open.target, id));
    }
    send(owner: string, target: NodeAddress, event: SerializedNodeEvent, id?: string): Promise<number | void> {
        if (!id) return this.env.send(target, event);
        const open = this.find(owner, target, id);
        if (!open) return Promise.reject(new Error("Node-event gesture ended or belongs to another view"));
        return this.queue(open, () => this.env.send(open.target, event, id));
    }
    end(owner: string, target: NodeAddress, id: string): Promise<number | void> {
        const open = this.find(owner, target, id);
        if (!open) return Promise.resolve();
        // Stop accepting new updates immediately, but close history only after
        // all requests already accepted by this owner have completed.
        this.gestures.delete(id);
        this.closing.add(open);
        return open.pending.catch(() => {}).then(async () => {
            if (!open.closed) return this.env.end(open.target, id);
        }).finally(() => this.closing.delete(open));
    }
    async releaseOwner(owner: string): Promise<void> {
        const closing: Promise<number | void>[] = [];
        for (const [id, open] of this.gestures) {
            if (open.owner === owner) closing.push(this.end(owner, open.target, id).catch((error) => this.env.report(error)));
        }
        await Promise.all(closing);
    }
    disconnected(): void {
        for (const open of this.gestures.values()) open.closed = true;
        for (const open of this.closing) open.closed = true;
        this.gestures.clear();
    }
    private find(owner: string, target: NodeAddress, id: string): OpenGesture | undefined {
        const open = this.gestures.get(id);
        return open?.owner === owner && addressKey(open.target) === addressKey(target) ? open : undefined;
    }
    private queue(open: OpenGesture, run: () => Promise<number | void>): Promise<number | void> {
        open.pending = open.pending.then(async () => {
            if (open.closed || open.failed) throw new Error("Node-event gesture ended or failed");
            return run();
        }).catch((error) => { open.failed = true; throw error; });
        // Cleanup still owns the rejected queue even if its originating view
        // vanished before attaching a completion handler.
        void open.pending.catch(() => {});
        return open.pending;
    }
}

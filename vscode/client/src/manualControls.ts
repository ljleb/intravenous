import { addressKey, type GraphScope, type NodeAddress } from "./graphScope";
import type { KnobGestureEvent } from "./knobAudioRecording";

export type ManualControlId = { node: NodeAddress; inputName: string };
export type ManualControlDescriptor = {
    value: number;
    enabled: boolean;
    participation: "disconnectedOnly" | "participateWithConnections";
    available: boolean;
    participates: boolean;
    // An open gesture on this control, whose value is not yet durable.
    live?: boolean;
    liveValue?: number;
};
// The latest value of a gesture this view does not own, for display only.
export type ManualControlLiveValue = { control: ManualControlId; value: number };
export type ManualControlAction =
    | { action: "hover"; control: ManualControlId | null }
    | { action: "begin"; interactionId: string; control: ManualControlId; value: number }
    | { action: "update" | "end"; interactionId: string; value: number };
export type ManualControlInteractionState = {
    interactionId: string;
    control: ManualControlId;
    value: number;
    phase: "starting" | "active" | "ending" | "settled" | "failed";
    error?: string;
};
export interface ManualControlTransport {
    hoverManualControl(control: ManualControlId | null): Promise<void>;
    beginManualControl(interactionId: string, control: ManualControlId, value: number): Promise<void>;
    // Unacknowledged: the server applies it at once or drops it after the end.
    updateManualControl(interactionId: string, value: number): void;
    endManualControl(interactionId: string, value: number): Promise<void>;
}
export function manualControlKey(control: ManualControlId): string {
    return JSON.stringify([addressKey(control.node), control.inputName]);
}
export function resolveManualControl(scope: GraphScope | null, id: ManualControlId) {
    const node = scope?.nodes.find((entry) => addressKey(entry.address) === addressKey(id.node));
    return node?.sampleInputs.find((port) => port.name === id.inputName)?.manualControl;
}

type Interaction = ManualControlInteractionState & {
    owner: string;
    remoteId: string;
    fallbackValue: number;
    sent: number;
    remoteActive: boolean;
    scheduled: boolean;
    ending: boolean;
    discardAfterEnd: boolean;
    epoch: number;
};

// Workspace-owned, independent of Vue and its component lifetimes. Hover, begin
// and end share one queue, so each mutation uses the revision returned by the
// preceding request. Once a begin is acknowledged, its pointer updates go out at
// once as unacknowledged notifications: they never wait for a reply, and the end
// carries the value that is committed.
export class ManualControlInteractions {
    private scope: GraphScope | null = null;
    private readonly entries = new Map<string, Interaction>();
    // Relayed by the server from other clients' gestures.
    private readonly remoteLive = new Map<string, ManualControlLiveValue>();
    // Set only while the knob-audio recorder runs.
    private observer: ((event: KnobGestureEvent) => void) | null = null;
    private tail: Promise<void> = Promise.resolve();
    private epoch = 0;
    private hovered: { owner: string; control: ManualControlId } | null = null;
    constructor(private readonly transport: () => ManualControlTransport | null,
        private readonly changed: () => void) {}

    observe(observer: ((event: KnobGestureEvent) => void) | null): void { this.observer = observer; }
    private report(kind: KnobGestureEvent["kind"], entry: Interaction, value: number): void {
        this.observer?.({ kind, interactionId: entry.interactionId, inputName: entry.control.inputName,
            value, clientMs: performance.now() });
    }
    states(owner: string): ManualControlInteractionState[] {
        return [...this.entries.values()].filter((entry) => entry.owner === owner).map((entry) => ({
            interactionId: entry.interactionId, control: entry.control, value: entry.value,
            phase: entry.phase, error: entry.error,
        }));
    }
    private key(owner: string, id: string) { return JSON.stringify([owner, id]); }
    private queue(task: () => Promise<void>): void {
        const epoch = this.epoch;
        this.tail = this.tail.then(async () => { if (epoch === this.epoch) await task(); });
    }
    async settled(): Promise<void> {
        for (;;) { const tail = this.tail; await tail; if (tail === this.tail) return; }
    }
    action(owner: string, action: ManualControlAction): void {
        if (action.action === "hover") { this.hover(owner, action.control); return; }
        if (!Number.isFinite(action.value)) throw new Error("A manual value must be finite");
        const key = this.key(owner, action.interactionId);
        if (action.action === "begin") {
            const descriptor = resolveManualControl(this.scope, action.control);
            if (!descriptor?.available || !descriptor.enabled) throw new Error("The manual input is unavailable");
            if (this.entries.has(key) && !["failed", "settled"].includes(this.entries.get(key)!.phase))
                throw new Error("This manual interaction is already active");
            if ([...this.entries.values()].some((entry) => entry.phase !== "failed" && entry.phase !== "settled"
                && manualControlKey(entry.control) === manualControlKey(action.control)
                && !(entry.owner === owner && entry.ending)))
                throw new Error("This input already has an active interaction");
            const predecessors = [...this.entries.values()].filter((entry) => entry.owner === owner && entry.ending
                && entry.phase !== "failed" && entry.phase !== "settled"
                && manualControlKey(entry.control) === manualControlKey(action.control));
            for (const [previousKey, previous] of this.entries)
                if (previous.owner === owner && (previous.phase === "failed" || previous.phase === "settled")
                    && manualControlKey(previous.control) === manualControlKey(action.control)) this.entries.delete(previousKey);
            const entry: Interaction = { ...action, owner, remoteId: key,
                fallbackValue: descriptor.value, sent: action.value, phase: "starting",
                remoteActive: false, scheduled: false, ending: false, discardAfterEnd: false, epoch: this.epoch };
            this.entries.set(key, entry);
            this.changed();
            this.queue(async () => {
                try {
                    const rpc = this.transport();
                    if (!rpc) throw new Error("The Intravenous server is unavailable");
                    // A quick re-grab or another keyboard step can arrive
                    // before the prior begin reply scheduled its end. Close
                    // those gestures before sending this begin; any queued
                    // old end then sees remoteActive=false and does nothing.
                    for (const previous of predecessors) {
                        if (previous.remoteActive) {
                            try { await rpc.endManualControl(previous.remoteId, previous.value); }
                            catch (error) { await this.fail(previous, error); throw error; }
                            if (entry.epoch !== this.epoch) return;
                            previous.fallbackValue = previous.sent = previous.value;
                            previous.remoteActive = false;
                            previous.phase = "settled";
                        }
                        entry.fallbackValue = previous.fallbackValue;
                        this.entries.delete(this.key(previous.owner, previous.interactionId));
                    }
                    const value = entry.value;
                    this.report("begin", entry, value);
                    await rpc.beginManualControl(entry.remoteId, entry.control, value);
                    if (entry.epoch !== this.epoch) return;
                    entry.remoteActive = true; entry.fallbackValue = value; entry.sent = value;
                    entry.phase = entry.ending ? "ending" : "active";
                    this.changed();
                    this.schedule(entry);
                } catch (error) { await this.fail(entry, error); }
            });
            return;
        }
        const entry = this.entries.get(key);
        if (!entry || entry.phase === "failed" || entry.phase === "settled") return; // Late pointer event after invalidation.
        entry.value = action.value;
        if (action.action === "end") { entry.ending = true; entry.phase = "ending"; }
        this.changed();
        this.schedule(entry);
    }
    private schedule(entry: Interaction): void {
        if (!entry.remoteActive || entry.scheduled || entry.phase === "failed") return;
        if (!entry.ending && entry.value === entry.sent) return;
        if (!entry.ending) {
            const rpc = this.transport();
            if (!rpc) { this.queue(() => this.fail(entry, new Error("The Intravenous server is unavailable"))); return; }
            try { rpc.updateManualControl(entry.remoteId, entry.value); this.report("update", entry, entry.value); }
            catch (error) { this.queue(() => this.fail(entry, error)); return; }
            entry.sent = entry.fallbackValue = entry.value;
            return;
        }
        entry.scheduled = true;
        this.queue(async () => {
            if (entry.phase === "failed" || !entry.remoteActive) { entry.scheduled = false; return; }
            const value = entry.value;
            try {
                const rpc = this.transport();
                if (!rpc) throw new Error("The Intravenous server is unavailable");
                this.report("end", entry, value);
                await rpc.endManualControl(entry.remoteId, value);
                if (entry.epoch !== this.epoch) return;
                entry.sent = value; entry.fallbackValue = value;
                entry.scheduled = false;
                entry.remoteActive = false;
                entry.phase = "settled";
                if (entry.discardAfterEnd) this.entries.delete(this.key(entry.owner, entry.interactionId));
                this.changed();
            } catch (error) { entry.scheduled = false; await this.fail(entry, error); }
        });
    }
    private async fail(entry: Interaction, error: unknown): Promise<void> {
        if (entry.epoch !== this.epoch) return;
        entry.phase = "failed";
        entry.value = entry.fallbackValue;
        entry.error = error instanceof Error ? error.message : String(error);
        // If the server gesture is still open, close it with the latest value
        // handed to the transport. Live notifications have no acknowledgement.
        if (entry.remoteActive) {
            try { await this.transport()?.endManualControl(entry.remoteId, entry.fallbackValue); }
            catch (cleanup) { entry.error += `; ending interaction: ${String(cleanup)}`; }
            entry.remoteActive = false;
        }
        if (entry.discardAfterEnd) this.entries.delete(this.key(entry.owner, entry.interactionId));
        this.changed();
    }
    private hover(owner: string, control: ManualControlId | null): void {
        if (!control && this.hovered?.owner !== owner) return;
        if (control) {
            const descriptor = resolveManualControl(this.scope, control);
            if (!descriptor?.available || !descriptor.enabled) return;
            if (this.hovered?.owner === owner && manualControlKey(this.hovered.control) === manualControlKey(control)) return;
            this.hovered = { owner, control };
        } else this.hovered = null;
        this.queue(async () => {
            try { await this.transport()?.hoverManualControl(control); }
            catch { /* A stale predictive hover does not reject a user edit. */ }
        });
    }
    // Values to display for gestures that `owner` does not hold: this client's
    // other views first, then values relayed from other clients.
    live(owner: string): ManualControlLiveValue[] {
        const values = new Map<string, ManualControlLiveValue>(this.remoteLive);
        for (const entry of this.entries.values()) {
            if (entry.owner === owner || entry.phase === "settled" || entry.phase === "failed") continue;
            values.set(manualControlKey(entry.control), { control: entry.control, value: entry.value });
        }
        return [...values.values()];
    }
    // One unreliable relayed value. The server sends none after the gesture's
    // committing revision, so a later scope read settles the display.
    receiveLive(value: ManualControlLiveValue): void {
        if (!Number.isFinite(value.value)) return;
        this.remoteLive.set(manualControlKey(value.control), value);
        this.changed();
    }
    reconcile(scope: GraphScope): void {
        this.scope = scope;
        for (const node of scope.nodes) {
            for (const port of node.sampleInputs) {
                const descriptor = port.manualControl;
                if (!descriptor) continue;
                const control = { node: node.address, inputName: port.name };
                const key = manualControlKey(control);
                if (!descriptor.live) this.remoteLive.delete(key);
                else if (descriptor.liveValue !== undefined && !this.remoteLive.has(key))
                    this.remoteLive.set(key, { control, value: descriptor.liveValue });
            }
        }
        for (const key of this.remoteLive.keys())
            if (!resolveManualControl(scope, this.remoteLive.get(key)!.control)?.live) this.remoteLive.delete(key);
        for (const entry of this.entries.values()) {
            const descriptor = resolveManualControl(scope, entry.control);
            if (entry.phase === "settled") {
                if (descriptor) entry.value = descriptor.value;
                else this.entries.delete(this.key(entry.owner, entry.interactionId));
                this.changed();
                continue;
            }
            if ((!descriptor?.available || !descriptor.enabled) && entry.phase !== "failed") {
                this.queue(() => this.fail(entry, new Error("The manual input was removed or became incompatible")));
            }
        }
        if (this.hovered && !resolveManualControl(scope, this.hovered.control)?.available)
            this.hover(this.hovered.owner, null);
    }
    releaseOwner(owner: string): void {
        this.hover(owner, null);
        for (const entry of this.entries.values()) {
            if (entry.owner !== owner) continue;
            if (entry.phase === "failed" || entry.phase === "settled") this.entries.delete(this.key(owner, entry.interactionId));
            else {
                entry.discardAfterEnd = true;
                this.action(owner, { action: "end", interactionId: entry.interactionId, value: entry.value });
            }
        }
    }
    disconnect(): void {
        ++this.epoch;
        this.scope = null; this.hovered = null; this.entries.clear(); this.remoteLive.clear(); this.tail = Promise.resolve();
        this.changed();
    }
}

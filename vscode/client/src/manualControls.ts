import { addressKey, type GraphScope, type NodeAddress } from "./graphScope";

export type ManualControlId = { node: NodeAddress; inputName: string };
export type ManualControlDescriptor = {
    value: number;
    enabled: boolean;
    participation: "disconnectedOnly" | "participateWithConnections";
    available: boolean;
    participates: boolean;
};
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
    updateManualControl(interactionId: string, value: number): Promise<void>;
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
    accepted: number;
    sent: number;
    remoteActive: boolean;
    scheduled: boolean;
    ending: boolean;
    discardAfterEnd: boolean;
    epoch: number;
};

// Workspace-owned, independent of Vue and its component lifetimes. All control
// requests share one queue, so each mutation uses the revision returned by the
// preceding request. Pointer updates retain only the latest unsent value.
export class ManualControlInteractions {
    private scope: GraphScope | null = null;
    private readonly entries = new Map<string, Interaction>();
    private tail: Promise<void> = Promise.resolve();
    private epoch = 0;
    private hovered: { owner: string; control: ManualControlId } | null = null;
    constructor(private readonly transport: () => ManualControlTransport | null,
        private readonly changed: () => void) {}

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
                accepted: descriptor.value, sent: action.value, phase: "starting",
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
                            previous.accepted = previous.sent = previous.value;
                            previous.remoteActive = false;
                            previous.phase = "settled";
                        }
                        entry.accepted = previous.accepted;
                        this.entries.delete(this.key(previous.owner, previous.interactionId));
                    }
                    const value = entry.value;
                    await rpc.beginManualControl(entry.remoteId, entry.control, value);
                    if (entry.epoch !== this.epoch) return;
                    entry.remoteActive = true; entry.accepted = value; entry.sent = value;
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
        entry.scheduled = true;
        this.queue(async () => {
            if (entry.phase === "failed" || !entry.remoteActive) { entry.scheduled = false; return; }
            const value = entry.value;
            const ending = entry.ending;
            try {
                const rpc = this.transport();
                if (!rpc) throw new Error("The Intravenous server is unavailable");
                if (ending) await rpc.endManualControl(entry.remoteId, value);
                else await rpc.updateManualControl(entry.remoteId, value);
                if (entry.epoch !== this.epoch) return;
                entry.sent = value; entry.accepted = value;
                entry.scheduled = false;
                if (ending) {
                    entry.remoteActive = false;
                    entry.phase = "settled";
                    if (entry.discardAfterEnd) this.entries.delete(this.key(entry.owner, entry.interactionId));
                } else this.schedule(entry);
                this.changed();
            } catch (error) { entry.scheduled = false; await this.fail(entry, error); }
        });
    }
    private async fail(entry: Interaction, error: unknown): Promise<void> {
        if (entry.epoch !== this.epoch) return;
        entry.phase = "failed";
        entry.value = entry.accepted;
        entry.error = error instanceof Error ? error.message : String(error);
        // A rejected update does not undo earlier accepted values. Close the
        // server gesture with its last acknowledged value, never replay it.
        if (entry.remoteActive) {
            try { await this.transport()?.endManualControl(entry.remoteId, entry.accepted); }
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
    reconcile(scope: GraphScope): void {
        this.scope = scope;
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
        this.scope = null; this.hovered = null; this.entries.clear(); this.tail = Promise.resolve();
        this.changed();
    }
}

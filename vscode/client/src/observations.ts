import type { NodeAddress } from "./graphScope";

export type ObservationSource = {
    node: NodeAddress;
    kind: "output" | "input" | "stateField";
    member: string;
    channel?: number;
};
export type MeterMode = "level" | "value";
export type ObservationWindow = { begin: string; length: string; points: number };
export type ObservationWindowResult = {
    id: string; window: ObservationWindow; status: "pending" | "ready" | "unavailable" | "failed";
    approximate: boolean; truncated?: boolean; outdated?: boolean; semanticVersion?: string; dataVersion?: string;
    sampleRate?: number; paceExponent?: number; executionGeneration?: string; error?: string;
    diagnostic?: string;
    regions: Array<{ begin: string; length: string;
        points: Array<{ index: string; available: boolean; value?: Record<string, number | string | null> }> }>;
};
export type ObservationInterest = { id: string; source: ObservationSource; operation: string; window?: ObservationWindow };
export type ObservationCapability = { nodeIndex: number; kind: ObservationSource["kind"]; member: string; operations: string[];
    delivery?: "window"; channels?: Array<{ index: number; name: string }>;
    resultSchemas?: Array<{ operation: string; fields: Array<{ name: string; type: "float32" | "float64" | "uint64String" }> }> };
export type ObservationUpdate = {
    activationId: string; generation: string; revision: string; index: string; blockSize: number;
    captureNs?: number | null;
    scopes: Array<{ scopeId: string; values: Array<{ id: string; available: boolean; pending?: boolean; value?: Record<string, number | string | null> }>; windows?: ObservationWindowResult[] }>;
};
export type ObservationHostRequest =
    | { type: "observationDescribe"; requestId: string; nodes: NodeAddress[] }
    | { type: "observationReconcile"; requestId: string; interests: ObservationInterest[] };
export type ObservationResponse = { type: "observationResponse"; requestId: string; result?: unknown; error?: string };
export type ObservationDisplayValue = { activationId?: string; available: boolean; pending?: boolean; value?: Record<string, number | string | null> };

export function observationSourceKey(source: ObservationSource): string {
    return JSON.stringify([source.node.instanceId, source.node.path ?? [], source.kind, source.member, source.channel ?? null]);
}

export function summarizeObservationWindow(result: ObservationWindowResult | undefined) {
    if (!result) return null;
    const { regions, ...window } = result;
    let sampled = 0, available = 0, finite = 0;
    for (const region of regions) for (const point of region.points) {
        ++sampled;
        if (!point.available) continue;
        ++available;
        if (typeof point.value?.value === "number" && Number.isFinite(point.value.value)) ++finite;
    }
    return { ...window, regions: regions.length, sampled, available, finite };
}

// A host scope replaces its complete desired set. It never lets an older
// activation or a result from an earlier request revision replace current data.
export class ObservationValues {
    private activation = 0n;
    private revision = 0n;
    private generation: string | null | undefined;
    private queuedUpdate?: { update: ObservationUpdate; scopeId: string };
    readonly values = new Map<string, ObservationDisplayValue>();
    readonly windows = new Map<string, ObservationWindowResult>();
    private desiredWindows = new Map<string, ObservationWindow>();
    deliveryState() {
        return { generation: this.generation, activationId: String(this.activation), revision: String(this.revision),
            queuedGeneration: this.queuedUpdate?.update.generation };
    }
    expectWindows(interests: ObservationInterest[]): void {
        this.desiredWindows = new Map(interests.filter((interest) => interest.window)
            .map((interest) => [interest.id, interest.window!]));
        for (const [id, result] of this.windows)
            if (!sameObservationWindow(result.window, this.desiredWindows.get(id))) this.windows.delete(id);
    }
    acceptGeneration(generation: number | null): boolean {
        const next = generation === null ? null : String(generation);
        if (next === this.generation) return false;
        this.generation = next;
        this.values.clear();
        this.windows.clear();
        const queued = this.queuedUpdate;
        if (queued && next !== null && BigInt(queued.update.generation) <= BigInt(next)) {
            this.queuedUpdate = undefined;
            this.receive(queued.update, queued.scopeId);
        }
        return true;
    }
    acceptRevision(revision: string): void { this.revision = BigInt(revision) > this.revision ? BigInt(revision) : this.revision; }
    receive(update: ObservationUpdate, scopeId: string): boolean {
        const activation = BigInt(update.activationId);
        if (activation < this.activation || BigInt(update.revision) < this.revision) return false;
        const scope = update.scopes.find((scope) => scope.scopeId === scopeId);
        if (!scope) return false;
        if (this.generation !== undefined && update.generation !== this.generation) {
            const queued = this.queuedUpdate?.update;
            // Observation notifications can overtake the graph scope read.
            // The server deduplicates unchanged batches, so keep the newest
            // one until its configured generation is known to the webview.
            if (BigInt(update.generation) > BigInt(this.generation ?? "0")
                && (!queued || BigInt(update.generation) >= BigInt(queued.generation)
                    && activation >= BigInt(queued.activationId) && BigInt(update.revision) >= BigInt(queued.revision)))
                this.queuedUpdate = { update, scopeId };
            return false;
        }
        this.activation = activation;
        for (const item of scope.values) this.values.set(item.id, { activationId: update.activationId,
            available: item.available, ...(item.pending ? { pending: true } : {}), value: item.value });
        for (const item of scope.windows ?? []) {
            if (!sameObservationWindow(item.window, this.desiredWindows.get(item.id))) continue;
            const previous = this.windows.get(item.id);
            if (previous?.status === "ready" && item.status === "ready"
                && (BigInt(item.semanticVersion ?? "0") < BigInt(previous.semanticVersion ?? "0")
                    || (item.semanticVersion === previous.semanticVersion
                        && BigInt(item.dataVersion ?? "0") < BigInt(previous.dataVersion ?? "0")))) continue;
            this.windows.set(item.id, item);
        }
        return true;
    }
    clear(): void {
        this.activation = 0n;
        this.revision = 0n;
        this.generation = undefined;
        this.queuedUpdate = undefined;
        this.values.clear();
        this.windows.clear();
        this.desiredWindows.clear();
    }
}

export function sameObservationWindow(left: ObservationWindow | undefined, right: ObservationWindow | undefined): boolean {
    return left?.begin === right?.begin && left?.length === right?.length && left?.points === right?.points;
}

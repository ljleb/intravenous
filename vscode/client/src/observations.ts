import type { NodeAddress } from "./graphScope";

export type ObservationSource = {
    node: NodeAddress;
    kind: "output" | "input" | "stateField";
    member: string;
    channel?: number;
    portKind?: "sample" | "event";
};
export type MeterMode = "level" | "value";
export type ObservationWindow = { begin: string; length: string; points: number; maxDepth?: number };
export type ObservationTimeSpan = Pick<ObservationWindow, "begin" | "length">;
export type ObservationWindowSnapshot = { epoch: number; generation: string; dataVersion: string };
export type ObservationWindowResult = {
    id: string; window: ObservationWindow; status: "pending" | "ready" | "unavailable" | "failed";
    approximate: boolean; truncated?: boolean; outdated?: boolean; semanticVersion?: string; dataVersion?: string;
    sampleRate?: number; paceExponent?: number; executionGeneration?: string; error?: string;
    marks?: Array<{ index: string; depth: number }>;
    unavailableRegions?: Array<{ begin: string; length: string }>;
    regions: Array<{ begin: string; length: string;
        points: Array<{ index: string; available: boolean; value?: Record<string, number | string | null> }> }>;
};
export type ObservationInterest = { id: string; source: ObservationSource; operation: string; window?: ObservationWindow; prefetch?: boolean };
export type ObservationCapability = { nodeIndex: number; kind: ObservationSource["kind"]; member: string; operations: string[];
    delivery?: "window"; channels?: Array<{ index: number; name: string }>;
    resultSchemas?: Array<{ operation: string; fields: Array<{ name: string; type: "float32" | "float64" | "uint64String" }> }> };
export type ObservationUpdate = {
    activationId: string; generation: string; revision: string; index: string; blockSize: number; dataVersion: string;
    captureNs?: number | null;
    scopes: Array<{ scopeId: string; values: Array<{ id: string; available: boolean; pending?: boolean; value?: Record<string, number | string | null> }>; windows?: ObservationWindowResult[] }>;
};
export type ObservationHostRequest =
    | { type: "observationDescribe"; requestId: string; nodes: NodeAddress[] }
    | { type: "observationReconcile"; requestId: string; interests: ObservationInterest[] };
export type ObservationResponse = { type: "observationResponse"; requestId: string; result?: unknown; error?: string };
export type ObservationDisplayValue = { activationId?: string; available: boolean; pending?: boolean; value?: Record<string, number | string | null> };

export function observationSourceKey(source: ObservationSource): string {
    return JSON.stringify([source.node.instanceId, source.node.path ?? [], source.kind, source.member, source.channel ?? null, ...(source.portKind === "event" ? ["event"] : [])]);
}

export function compareObservationWindowVersions(left: ObservationWindowResult, right: ObservationWindowResult): number {
    for (const field of ["semanticVersion", "dataVersion", "executionGeneration"] as const) {
        const a = BigInt(left[field] ?? "0"), b = BigInt(right[field] ?? "0");
        if (a !== b) return a > b ? 1 : -1;
    }
    return 0;
}

// A host scope replaces its complete desired set. It never lets an older
// activation or a result from an earlier request revision replace current data.
export class ObservationValues {
    private activation = 0n;
    private revision = 0n;
    private generation: string | null | undefined;
    private dataVersion: string | undefined;
    private windowEpoch = 0;
    private queuedUpdate?: { update: ObservationUpdate; scopeId: string };
    readonly values = new Map<string, ObservationDisplayValue>();
    readonly windows = new Map<string, ObservationWindowResult>();
    private desiredWindows = new Map<string, { source: string; window: ObservationWindow }>();
    private readyWindows = new Map<string, ObservationWindowResult>();
    get windowSnapshot(): ObservationWindowSnapshot | undefined {
        return this.generation != null && this.dataVersion !== undefined
            ? { epoch: this.windowEpoch, generation: this.generation, dataVersion: this.dataVersion } : undefined;
    }
    expectWindows(interests: ObservationInterest[]): void {
        const next = new Map(interests.filter((interest) => interest.window).map((interest) => [interest.id, {
            source: JSON.stringify([observationSourceKey(interest.source), interest.operation]), window: interest.window!,
        }]));
        for (const [id, previous] of this.desiredWindows) {
            const wanted = next.get(id);
            if (!wanted || previous.source !== wanted.source || previous.window.maxDepth !== wanted.window.maxDepth)
                this.forgetWindow(id);
        }
        this.desiredWindows = next;
        for (const [id, result] of this.windows)
            if (!sameObservationWindow(result.window, next.get(id)?.window)) this.windows.delete(id);
    }
    forgetWindow(id: string): void {
        this.desiredWindows.delete(id);
        this.windows.delete(id);
        this.readyWindows.delete(id);
    }
    windowForDisplay(id: string, visible: ObservationTimeSpan | undefined = this.desiredWindows.get(id)?.window): ObservationWindowResult | undefined {
        if (!visible) return;
        const current = this.windows.get(id);
        const aligned = current?.window.begin === visible.begin && current.window.length === visible.length ? current : undefined;
        if (aligned && aligned.status !== "pending") return aligned;
        const ready = this.readyWindows.get(id);
        // One last ready window per active interest. Consumers project its
        // absolute positions through the current viewport and clip to overlap.
        if (ready) {
            const begin = BigInt(visible.begin), old = BigInt(ready.window.begin);
            if (BigInt.asUintN(64, old - begin) < BigInt(visible.length)
                || BigInt.asUintN(64, begin - old) < BigInt(ready.window.length))
                return ready.outdated ? ready : { ...ready, outdated: true };
        }
        return aligned;
    }
    acceptGeneration(generation: number | null): boolean {
        const next = generation === null ? null : String(generation);
        if (next === this.generation) return false;
        this.generation = next;
        this.dataVersion = undefined;
        ++this.windowEpoch;
        this.values.clear();
        this.windows.clear();
        this.readyWindows.clear();
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
        this.dataVersion = update.dataVersion;
        const liveIds = new Set(scope.values.map(({ id }) => id));
        const windowIds = new Set(scope.windows?.map(({ id }) => id));
        for (const id of this.values.keys()) if (!liveIds.has(id)) this.values.delete(id);
        for (const id of this.windows.keys()) if (!windowIds.has(id)) this.windows.delete(id);
        for (const item of scope.values) this.values.set(item.id, { activationId: update.activationId,
            available: item.available, ...(item.pending ? { pending: true } : {}), value: item.value });
        for (const item of scope.windows ?? []) {
            if (!sameObservationWindow(item.window, this.desiredWindows.get(item.id)?.window)) continue;
            const previous = this.readyWindows.get(item.id);
            if (previous && item.status === "ready" && sameObservationWindow(item.window, previous.window)
                && compareObservationWindowVersions(item, previous) < 0) continue;
            this.windows.set(item.id, item);
            if (item.status === "ready") this.readyWindows.set(item.id, item);
            else if (item.status !== "pending") this.readyWindows.delete(item.id);
        }
        return true;
    }
    clear(): void {
        this.activation = 0n;
        this.revision = 0n;
        this.generation = undefined;
        this.dataVersion = undefined;
        ++this.windowEpoch;
        this.queuedUpdate = undefined;
        this.values.clear();
        this.windows.clear();
        this.readyWindows.clear();
        this.desiredWindows.clear();
    }
}

export function sameObservationWindow(left: ObservationWindow | undefined, right: ObservationWindow | undefined): boolean {
    return left?.begin === right?.begin && left?.length === right?.length && left?.points === right?.points && left?.maxDepth === right?.maxDepth;
}

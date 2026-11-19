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

type ReadyWindow = { generation: string; result: ObservationWindowResult };
const retainedWindowsPerInterest = 2;

function windowsOverlap(left: ObservationTimeSpan, right: ObservationTimeSpan): boolean {
    const start = BigInt(left.begin) - BigInt(right.begin);
    return BigInt.asUintN(64, start) < BigInt(right.length)
        || BigInt.asUintN(64, -start) < BigInt(left.length);
}

function compareReadyWindows(left: ReadyWindow, right: ReadyWindow): number {
    if (left.generation !== right.generation) return BigInt(left.generation) > BigInt(right.generation) ? 1 : -1;
    return compareObservationWindowVersions(left.result, right.result);
}

// A host scope replaces its complete desired set. It never lets an older
// activation or a result from an earlier request revision replace current data.
export class ObservationValues {
    private activation = 0n;
    private revision = 0n;
    private generation: string | null | undefined;
    private sampleRate: number | undefined;
    private queuedUpdate?: { update: ObservationUpdate; scopeId: string };
    readonly values = new Map<string, ObservationDisplayValue>();
    private windows = new Map<string, ObservationWindowResult>();
    private desiredWindows = new Map<string, { source: string; window: ObservationWindow }>();
    private readyWindows = new Map<string, ReadyWindow[]>();
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
        if (aligned?.status === "ready" || aligned?.status === "unavailable") return aligned;
        const ready = this.readyWindows.get(id)?.find(({ result }) => windowsOverlap(result.window, visible));
        if (ready) return { ...ready.result, outdated: true, ...(aligned?.error ? { error: aligned.error } : {}) };
        return aligned;
    }

    readyWindowsForDisplay(ids: readonly string[], visible: ObservationTimeSpan, completeOnly = false): ObservationWindowResult[] {
        const selected = new Map<string, ReadyWindow>();
        for (const id of ids) for (const ready of this.readyWindows.get(id) ?? []) {
            const { result } = ready;
            if ((completeOnly && result.truncated) || !windowsOverlap(result.window, visible)) continue;
            const { begin, length, points, maxDepth } = result.window;
            const key = JSON.stringify([begin, length, points, maxDepth]);
            const previous = selected.get(key);
            if (!previous || compareReadyWindows(ready, previous) >= 0) selected.set(key, ready);
        }
        return [...selected.values()].map(({ result }) =>
            this.windows.get(result.id) === result || result.outdated ? result : { ...result, outdated: true });
    }

    acceptGeneration(generation: number | null, sampleRate?: number | null): boolean {
        const next = generation === null ? null : String(generation);
        const rateChanged = this.sampleRate !== undefined && sampleRate != null && sampleRate !== this.sampleRate;
        if (sampleRate != null) this.sampleRate = sampleRate;
        if (next === this.generation && !rateChanged) return false;
        this.generation = next;
        this.values.clear();
        this.windows.clear();
        // Keep display history while the next configured graph prepares a
        // reply. A different sample rate changes the window's coordinates.
        if (rateChanged) this.readyWindows.clear();
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
        const liveIds = new Set(scope.values.map(({ id }) => id));
        const windowIds = new Set(scope.windows?.map(({ id }) => id));
        for (const id of this.values.keys()) if (!liveIds.has(id)) this.values.delete(id);
        for (const id of this.windows.keys()) if (!windowIds.has(id)) this.windows.delete(id);
        for (const item of scope.values) this.values.set(item.id, { activationId: update.activationId,
            available: item.available, ...(item.pending ? { pending: true } : {}), value: item.value });
        for (const item of scope.windows ?? []) {
            if (!sameObservationWindow(item.window, this.desiredWindows.get(item.id)?.window)) continue;
            if (item.status === "ready") {
                if (item.sampleRate && this.sampleRate !== undefined && item.sampleRate !== this.sampleRate) continue;
                const recent = this.readyWindows.get(item.id) ?? [];
                const previous = recent.findIndex(({ result }) => sameObservationWindow(result.window, item.window)
                    && !!result.truncated === !!item.truncated);
                if (previous >= 0) {
                    const cached = recent[previous];
                    if (cached.generation === update.generation && compareObservationWindowVersions(item, cached.result) < 0) continue;
                    recent.splice(previous, 1);
                }
                recent.unshift({ generation: update.generation, result: item });
                if (recent.length > retainedWindowsPerInterest) recent.pop();
                this.readyWindows.set(item.id, recent);
            } else if (item.status === "unavailable") this.readyWindows.delete(item.id);
            this.windows.set(item.id, item);
        }
        return true;
    }
    clear(): void {
        this.activation = 0n;
        this.revision = 0n;
        this.generation = undefined;
        this.sampleRate = undefined;
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

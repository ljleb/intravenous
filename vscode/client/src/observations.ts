import type { NodeAddress } from "./graphScope";
import { ObservationDeliveryState, type ObservationDelivery } from "./observationDelivery";

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
    gainApplied?: boolean;
    resultSchemas?: Array<{ operation: string; fields: Array<{ name: string; type: "float32" | "float64" | "uint64String" }> }> };
export type ObservationHostRequest =
    | { type: "observationDescribe"; requestId: string; nodes: NodeAddress[] }
    | { type: "observationReconcile"; requestId: string; interests: ObservationInterest[]; predictedInterests?: ObservationInterest[] };
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

type ReadyWindow = { generation: bigint; result: ObservationWindowResult; source: string; units: number };
type ObservationFrame = { activation: bigint; revision: bigint; generation: bigint };
const retainedWindowsPerInterest = 8;
const retainedWindowLimit = 512;
// Bound the display cache across the webview. Count samples, marks, and
// Regions separately because a short sparse result can contain many Regions.
const retainedWindowUnitsLimit = 65536;
const retainedLiveLimit = 1024;

function windowsOverlap(left: ObservationTimeSpan, right: ObservationTimeSpan): boolean {
    const start = BigInt(left.begin) - BigInt(right.begin);
    return BigInt.asUintN(64, start) < BigInt(right.length)
        || BigInt.asUintN(64, -start) < BigInt(left.length);
}

function compareReadyWindows(left: ReadyWindow, right: ReadyWindow): number {
    if (left.generation !== right.generation) return left.generation > right.generation ? 1 : -1;
    return compareObservationWindowVersions(left.result, right.result);
}

function windowSource({ source, operation, window }: Pick<ObservationInterest, "source" | "operation" | "window">): string {
    return JSON.stringify([observationSourceKey(source), operation, window?.maxDepth]);
}

// A host scope replaces its complete desired set. It never lets an older
// activation or a result from an earlier request revision replace current data.
export class ObservationValues {
    private revision = 0n;
    private generation: bigint | null | undefined;
    private sampleRate: number | undefined;
    private readonly delivery = new ObservationDeliveryState();
    private frame?: ObservationFrame;
    private needsSnapshot = true;
    readonly values: Map<string, ObservationDisplayValue>;
    private readonly lastLive = new Map<string, ObservationDisplayValue>();
    private readonly windows: Map<string, ObservationWindowResult>;
    private readonly desiredWindows: Map<string, { source: string; window: ObservationWindow }>;
    private readonly readyWindows: Map<string, ReadyWindow[]>;
    private readonly recentWindows = new Set<ReadyWindow>();
    private retainedWindowUnits = 0;
    constructor(private readonly scopeId = "viewport", track: <T>(map: Map<string, T>) => Map<string, T> = (map) => map) {
        this.values = track(new Map());
        this.windows = track(new Map());
        this.desiredWindows = track(new Map());
        this.readyWindows = track(new Map());
    }
    expectWindows(interests: ObservationInterest[]): void {
        const next = new Map(interests.filter((interest) => interest.window).map((interest) => [interest.id, {
            source: windowSource(interest), window: interest.window!,
        }]));
        for (const [id, previous] of this.desiredWindows) {
            const wanted = next.get(id);
            if (!wanted) this.retireWindow(id);
            else if (previous.source !== wanted.source) this.forgetWindow(id);
        }
        for (const [id, result] of this.windows)
            if (!sameObservationWindow(result.window, next.get(id)?.window)) this.windows.delete(id);
        const frame = this.currentFrame();
        for (const [id, wanted] of next) {
            this.dropIncompatibleWindows(id, wanted.source);
            const previous = this.desiredWindows.get(id);
            if (sameObservationWindow(previous?.window, wanted.window)) continue;
            this.desiredWindows.set(id, wanted);
            // A rapid pan back can restore a previously requested window
            // before the server changes its result. There is no new delta.
            if (frame && previous?.source === wanted.source) {
                const result = this.delivery.window(this.scopeId, id);
                if (result) this.applyWindow(result, frame.generation);
            }
        }
    }
    retireWindow(id: string): void {
        this.desiredWindows.delete(id);
        this.windows.delete(id);
    }
    private forgetWindow(id: string): void {
        this.retireWindow(id);
        this.removeReadyWindows(id, () => true);
    }
    prepareWindow(id: string, request: Pick<ObservationInterest, "source" | "operation" | "window">): void {
        this.dropIncompatibleWindows(id, windowSource(request));
    }
    private dropIncompatibleWindows(id: string, source: string): void {
        const recent = this.readyWindows.get(id);
        if (recent?.length && recent[0].source !== source) this.removeReadyWindows(id, () => true);
    }
    private removeReadyWindows(id: string, remove: (entry: ReadyWindow) => boolean): void {
        const entries = this.readyWindows.get(id);
        if (!entries?.some(remove)) return;
        const keep = entries.filter((entry) => {
            if (!remove(entry)) return true;
            this.recentWindows.delete(entry);
            this.retainedWindowUnits -= entry.units;
            return false;
        });
        if (keep.length) this.readyWindows.set(id, keep);
        else this.readyWindows.delete(id);
    }
    private useReadyWindow(entry: ReadyWindow): void {
        if (this.recentWindows.delete(entry)) this.recentWindows.add(entry);
    }
    private clearReadyWindows(): void {
        this.readyWindows.clear();
        this.recentWindows.clear();
        this.retainedWindowUnits = 0;
    }
    previousLiveValue(id: string): ObservationDisplayValue | undefined { return this.lastLive.get(id); }
    currentWindow(id: string, window: ObservationWindow): ObservationWindowResult | undefined {
        const current = this.windows.get(id);
        return current && sameObservationWindow(current.window, window) ? current : undefined;
    }
    windowForDisplay(id: string, visible: ObservationTimeSpan | undefined = this.desiredWindows.get(id)?.window): ObservationWindowResult | undefined {
        if (!visible) return;
        const current = this.windows.get(id);
        const aligned = current?.window.begin === visible.begin && current.window.length === visible.length ? current : undefined;
        if (aligned?.status === "ready" || aligned?.status === "unavailable") return aligned;
        const ready = this.readyWindows.get(id)?.find(({ result }) => windowsOverlap(result.window, visible));
        if (ready) {
            this.useReadyWindow(ready);
            return { ...ready.result, outdated: true, ...(aligned?.error ? { error: aligned.error } : {}) };
        }
        return aligned;
    }

    readyWindowsForDisplay(ids: readonly string[], visible: ObservationTimeSpan, completeOnly = false): ObservationWindowResult[] {
        const selected = new Map<string, ReadyWindow>();
        const consider = (ready: ReadyWindow) => {
            const { result } = ready;
            if ((completeOnly && result.truncated) || !windowsOverlap(result.window, visible)) return;
            const { begin, length, points, maxDepth } = result.window;
            const key = JSON.stringify([begin, length, points, maxDepth]);
            const previous = selected.get(key);
            if (!previous || compareReadyWindows(ready, previous) >= 0) selected.set(key, ready);
        };
        for (const id of ids) {
            const cached = this.readyWindows.get(id) ?? [];
            for (const ready of cached) consider(ready);
            const result = this.windows.get(id);
            if (result?.status === "ready" && !cached.some((entry) => entry.result === result)) consider({ generation: this.frame?.generation ?? 0n,
                source: "", units: 0, result });
        }
        return [...selected.values()].map((entry) => {
            this.useReadyWindow(entry);
            const { result } = entry;
            return this.windows.get(result.id) === result || result.outdated ? result : { ...result, outdated: true };
        });
    }

    acceptGeneration(generation: number | null, sampleRate?: number | null, onUsable?: (id: string) => void): boolean {
        const next = generation === null ? null : BigInt(generation);
        const rateChanged = this.sampleRate !== undefined && sampleRate != null && sampleRate !== this.sampleRate;
        if (sampleRate != null) this.sampleRate = sampleRate;
        if (next === this.generation && !rateChanged) return false;
        this.generation = next;
        this.values.clear();
        this.windows.clear();
        // Keep display history while the next configured graph prepares a
        // reply. A different sample rate changes the window's coordinates.
        if (rateChanged) this.clearReadyWindows();
        this.needsSnapshot = true;
        this.project(undefined, onUsable);
        return true;
    }
    acceptRevision(revision: string): void { this.revision = BigInt(revision) > this.revision ? BigInt(revision) : this.revision; }
    receive(update: ObservationDelivery, onUsable?: (id: string) => void): boolean {
        const frame = { activation: BigInt(update.activationId), revision: BigInt(update.revision), generation: BigInt(update.generation) };
        if ((this.generation != null && frame.generation < this.generation)
            || (this.frame && (frame.activation < this.frame.activation || frame.generation < this.frame.generation
                || frame.revision < this.frame.revision))) return false;
        this.delivery.receive(update);
        this.frame = frame;
        return this.project(update, onUsable);
    }
    private currentFrame(): ObservationFrame | undefined {
        const frame = this.frame;
        if (frame && frame.revision >= this.revision && (this.generation === undefined || this.generation === frame.generation)) return frame;
    }
    private project(update: ObservationDelivery | undefined, onUsable?: (id: string) => void): boolean {
        const frame = this.currentFrame();
        if (!frame) {
            // Keep merging transport patches while the graph or requirements
            // catch up. Project the complete scope once when it can be used.
            this.needsSnapshot = true;
            return false;
        }
        const scope = this.delivery.changes(this.scopeId, this.needsSnapshot ? undefined : update);
        this.needsSnapshot = false;
        let changed = false;
        if (scope.snapshot) {
            const liveIds = new Set(scope.values.map(([id]) => id));
            const windowIds = new Set(scope.windows.map(([id]) => id));
            for (const id of this.values.keys()) if (!liveIds.has(id) && this.values.delete(id)) changed = true;
            for (const id of this.windows.keys()) if (!windowIds.has(id) && this.windows.delete(id)) changed = true;
        }
        for (const [id, item] of scope.values) {
            if (!item) { if (this.values.delete(id)) changed = true; continue; }
            const reading = { activationId: String(frame.activation), ...item };
            if (!item.pending) {
                this.lastLive.delete(id);
                if (item.available) this.lastLive.set(id, reading);
                if (this.lastLive.size > retainedLiveLimit) this.lastLive.delete(this.lastLive.keys().next().value!);
            }
            this.values.set(id, reading);
            changed = true;
            if (item.available && !item.pending) onUsable?.(id);
        }
        for (const [id, item] of scope.windows) {
            if (!item) { if (this.windows.delete(id)) changed = true; continue; }
            if (!this.applyWindow(item, frame.generation)) continue;
            changed = true;
            if (item.status === "ready" && !item.outdated && !item.truncated) onUsable?.(id);
        }
        return changed;
    }
    private applyWindow(item: ObservationWindowResult, generation: bigint): boolean {
        const { id } = item;
        const desired = this.desiredWindows.get(id);
        if (!desired || !sameObservationWindow(item.window, desired.window)) return false;
        if (item.status === "ready") {
            if (item.sampleRate && this.sampleRate !== undefined && item.sampleRate !== this.sampleRate) return false;
            const previous = this.readyWindows.get(id)?.find(({ result }) => sameObservationWindow(result.window, item.window)
                && !!result.truncated === !!item.truncated);
            const current = this.currentWindow(id, item.window);
            if (current?.status === "ready" && !!current.truncated === !!item.truncated
                && compareObservationWindowVersions(item, current) < 0) return false;
            if (previous) {
                if (previous.generation === generation && compareObservationWindowVersions(item, previous.result) < 0) return false;
                this.removeReadyWindows(id, (entry) => entry === previous);
            }
            const units = 1 + (item.marks?.length ?? 0) + (item.unavailableRegions?.length ?? 0)
                + item.regions.reduce((count, region) => count + 1 + region.points.length, 0);
            if (units <= retainedWindowUnitsLimit) {
                const entry = { generation, source: desired.source, units, result: item };
                const recent = this.readyWindows.get(id) ?? [];
                this.readyWindows.set(id, [entry, ...recent]);
                this.recentWindows.add(entry);
                this.retainedWindowUnits += units;
                if (recent.length >= retainedWindowsPerInterest)
                    this.removeReadyWindows(id, (entry) => entry === recent.at(-1));
                while (this.recentWindows.size > retainedWindowLimit || this.retainedWindowUnits > retainedWindowUnitsLimit) {
                    const oldest = this.recentWindows.values().next().value!;
                    this.removeReadyWindows(oldest.result.id, (entry) => entry === oldest);
                }
            }
        } else if (item.status === "unavailable") {
            this.removeReadyWindows(id, ({ result }) => sameObservationWindow(result.window, item.window));
        }
        this.windows.set(id, item);
        return true;
    }
    clear(): void {
        this.revision = 0n;
        this.generation = undefined;
        this.sampleRate = undefined;
        this.frame = undefined;
        this.delivery.clear();
        this.needsSnapshot = true;
        this.values.clear();
        this.lastLive.clear();
        this.windows.clear();
        this.clearReadyWindows();
        this.desiredWindows.clear();
    }
}

export function sameObservationWindow(left: ObservationWindow | undefined, right: ObservationWindow | undefined): boolean {
    return left?.begin === right?.begin && left?.length === right?.length && left?.points === right?.points && left?.maxDepth === right?.maxDepth;
}

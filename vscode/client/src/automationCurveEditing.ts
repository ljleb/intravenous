import { addressKey, type GraphScope, type GraphScopeNode, type NodeAddress, type Point as ScreenPoint } from "./graphScope";
import { AddPoint, MovePoint, RemovePoint } from "./generated/automationEvents";
import type { NodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";

export type AutomationPoint = { id: number; seconds: number; value: number };
// Time is cyclic: past its last point the curve continues toward its first one,
// across the global timeline's wrap. cycleSeconds is that cycle, 2^64 samples at
// the accepted graph's sample rate, or Infinity before a graph has compiled.
export type AutomationCurve = { points: AutomationPoint[]; cycleSeconds: number };
export type CurveAxes = { left: number; top: number; width: number; height: number;
    begin: number; end: number; min: number; max: number };
type Transport = { send(target: NodeAddress, event: NodeEvent): Promise<void>; gesture(target: NodeAddress): NodeEventGesture };
type Timers = { set(callback: () => void): unknown; clear(handle: unknown): void };
type Drag = { key: string; target: NodeAddress; pointerId: number; axes: CurveAxes; desired: AutomationPoint;
    offset: { seconds: number; value: number };
    lastPosition?: ScreenPoint;
    sent: AutomationPoint | null; gesture: NodeEventGesture; timer?: unknown; pending?: Promise<void>;
    finishing?: Promise<void> };

const indexes = new WeakMap<AutomationCurve, { order: Map<number, number>; min: number; max: number }>();
export function automationCurveBounds(curve: AutomationCurve) {
    let index = indexes.get(curve);
    if (!index) {
        index = { order: new Map(), min: 0, max: 1 };
        curve.points.forEach((point, position) => {
            index!.order.set(point.id, position);
            index!.min = Math.min(index!.min, point.value); index!.max = Math.max(index!.max, point.value);
        });
        indexes.set(curve, index);
    }
    return index;
}

// Interpolates between two successive points on the cycle. A zero span is the
// full cycle, so one point, or several at one time, define a constant curve.
// Like the backend, it measures from the nearer endpoint to keep precision
// beside the wrap; with an unbounded cycle the wrap segment is level.
function cyclicValue(before: AutomationPoint, after: AutomationPoint, seconds: number, cycle: number): number {
    const forward = (from: number, to: number) => to >= from ? to - from : to - from + cycle;
    const distance = forward(before.seconds, after.seconds) || cycle;
    const elapsed = forward(before.seconds, seconds);
    const remaining = forward(seconds, after.seconds);
    return elapsed <= remaining
        ? before.value + (after.value - before.value) * (elapsed / distance)
        : after.value + (before.value - after.value) * (remaining / distance);
}

export function automationWindow(curve: AutomationCurve, begin: number, end: number, drafts: readonly AutomationPoint[] = []) {
    const boundary = (seconds: number, after: boolean) => {
        let first = 0, last = curve.points.length;
        while (first < last) {
            const middle = first + Math.floor((last - first) / 2);
            if (curve.points[middle].seconds < seconds || (after && curve.points[middle].seconds === seconds)) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    // A moved point can remove a neighbor from its old position. Include one
    // extra neighbor per pending draft, without traversing the full curve.
    const neighbors = drafts.length + 1;
    const ids = new Set(drafts.map((point) => point.id));
    const order = automationCurveBounds(curve).order;
    const effective = (slice: AutomationPoint[]) => slice.filter((point) => !ids.has(point.id)).concat(drafts)
        .sort((a, b) => a.seconds - b.seconds || order.get(a.id)! - order.get(b.id)!);
    const points = effective(curve.points.slice(Math.max(0, boundary(begin, false) - neighbors), boundary(end, true) + neighbors));
    // The wrap segment joins the curve's last point to its first.
    const first = effective(curve.points.slice(0, neighbors))[0];
    const last = effective(curve.points.slice(-neighbors)).at(-1);
    const sample = (seconds: number) => {
        if (!first || !last) return 0;
        let index = 0;
        while (index < points.length && points[index].seconds <= seconds) ++index;
        const inside = index > 0 && index < points.length;
        return cyclicValue(inside ? points[index - 1] : last, inside ? points[index] : first, seconds, curve.cycleSeconds);
    };
    const visible = points.filter((point) => point.seconds >= begin && point.seconds <= end);
    const vertices = [{ seconds: begin, value: sample(begin) },
        ...visible.map(({ seconds, value }) => ({ seconds, value })), { seconds: end, value: sample(end) }];
    return { points: visible, vertices };
}

export function automationCycleSeconds(sampleRate: number | null | undefined): number {
    return sampleRate && sampleRate > 0 ? 2 ** 64 / sampleRate : Infinity;
}

export function readAutomationCurve(value: GraphScopeNode["construction"], sampleRate?: number | null): AutomationCurve | null {
    if (value?.definitionId !== "iv.builtin.automation.curve" || value.arguments.length !== 1) return null;
    const [points] = value.arguments;
    if (!Array.isArray(points)) return null;
    const ids = new Set<number>();
    const result: AutomationPoint[] = [];
    for (const point of points) {
        if (!point || !Number.isInteger(point.id) || point.id < 0 || point.id >= 2 ** 32 || ids.has(point.id)
            || typeof point.seconds !== "number" || !Number.isFinite(point.seconds)
            || typeof point.value !== "number" || !Number.isFinite(point.value) || !Number.isFinite(Math.fround(point.value))) return null;
        ids.add(point.id); result.push({ id: point.id, seconds: point.seconds, value: point.value });
    }
    // Stable, like the module's constructor: equal times keep their list order.
    result.sort((a, b) => a.seconds - b.seconds);
    return { points: result, cycleSeconds: automationCycleSeconds(sampleRate) };
}

export function curvePointAt(axes: CurveAxes, position: ScreenPoint): { seconds: number; value: number } {
    return { seconds: axes.begin + (position.x - axes.left) / axes.width * (axes.end - axes.begin),
        value: axes.max - (position.y - axes.top) / axes.height * (axes.max - axes.min) };
}

// A webview owns the accepted read model and one local point draft. Components
// can remount while the pointer and the coalesced RPC remain owned here.
export class AutomationCurveEditing {
    private readonly curves = new Map<string, AutomationCurve>();
    private readonly errors = new Map<string, string>();
    private readonly ending = new Map<string, Map<number, { point: AutomationPoint; revision: number; axes: CurveAxes }>>();
    private readRevision = 0;
    private active: Drag | null = null;
    constructor(private readonly transport: Transport, private readonly changed: () => void,
        private readonly timers: Timers = { set: (callback) => setTimeout(callback, 80), clear: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>) }) {}
    reconcile(scope: GraphScope | null): void {
        if (scope?.sessionRevision !== undefined && scope.sessionRevision < this.readRevision) return;
        this.readRevision = scope?.sessionRevision ?? this.readRevision;
        const next = new Map<string, AutomationCurve>();
        for (const node of scope?.nodes ?? []) {
            // Desired configuration can fail while an accepted invocation is
            // still current. Its constructor values remain authoritative.
            const curve = readAutomationCurve(node.construction, scope?.sampleRate);
            if (curve) next.set(addressKey(node.address), curve);
        }
        this.curves.clear();
        for (const [key, curve] of next) this.curves.set(key, curve);
        for (const [key, entries] of this.ending) {
            if (!next.has(key)) { this.ending.delete(key); continue; }
            for (const [id, entry] of entries) if (this.readRevision >= entry.revision) entries.delete(id);
            if (!entries.size) this.ending.delete(key);
        }
        for (const key of this.errors.keys()) if (!next.has(key)) this.errors.delete(key);
        if (this.active && (!next.has(this.active.key)
            || !next.get(this.active.key)!.points.some((point) => point.id === this.active!.desired.id))) {
            this.abandon();
        }
        this.changed();
    }
    view(target: NodeAddress): AutomationCurve | null {
        return this.curves.get(addressKey(target)) ?? null;
    }
    drafts(target: NodeAddress): AutomationPoint[] {
        const key = addressKey(target);
        const points = new Map<number, AutomationPoint>();
        for (const entry of this.ending.get(key)?.values() ?? []) points.set(entry.point.id, entry.point);
        if (this.active?.key === key) points.set(this.active.desired.id, this.active.desired);
        return [...points.values()];
    }
    error(target: NodeAddress): string { return this.errors.get(addressKey(target)) ?? ""; }
    axes(target: NodeAddress): CurveAxes | null {
        const key = addressKey(target);
        return this.active?.key === key ? this.active.axes : this.ending.get(key)?.values().next().value?.axes ?? null;
    }
    pointerId(): number | null { return this.active?.finishing ? null : this.active?.pointerId ?? null; }
    begin(target: NodeAddress, id: number, pointerId: number, axes: CurveAxes, position?: ScreenPoint): boolean {
        if (this.active || axes.width <= 0 || axes.height <= 0) return false;
        const key = addressKey(target);
        const point = this.curves.get(key)?.points.find((entry) => entry.id === id);
        if (!point) return false;
        // A re-grab starts from the value still displayed while its accepted
        // read-model update is in transit, rather than jumping to an old read.
        const initial = this.ending.get(key)?.get(id)?.point ?? point;
        const cursor = position ? curvePointAt(axes, position) : initial;
        this.errors.delete(key);
        this.active = { key, target: structuredClone(target), pointerId, axes: { ...axes }, desired: { ...initial },
            offset: { seconds: initial.seconds - cursor.seconds, value: initial.value - cursor.value },
            lastPosition: position ? { ...position } : undefined,
            sent: { ...initial }, gesture: this.transport.gesture(target) };
        this.changed(); return true;
    }
    move(pointerId: number, position: ScreenPoint): void {
        const drag = this.active;
        if (!drag || drag.finishing || drag.pointerId !== pointerId) return;
        if (drag.lastPosition?.x === position.x && drag.lastPosition.y === position.y) return;
        drag.lastPosition = { ...position };
        const curve = this.curves.get(drag.key);
        if (!curve) return;
        const next = curvePointAt(drag.axes, position);
        next.seconds += drag.offset.seconds;
        next.value += drag.offset.value;
        if (!Number.isFinite(next.value) || !Number.isFinite(next.seconds)) return;
        drag.desired = { id: drag.desired.id, ...next };
        if (drag.timer === undefined && !drag.pending) drag.timer = this.timers.set(() => {
            drag.timer = undefined;
            void this.flush(drag).catch(() => {});
        });
        this.changed();
    }
    private flush(drag: Drag): Promise<void> {
        if (drag.pending) return drag.pending;
        drag.pending = (async () => {
            while (this.active === drag && (drag.sent?.seconds !== drag.desired.seconds || drag.sent?.value !== drag.desired.value)) {
                const point = { ...drag.desired };
                await drag.gesture.send(MovePoint(point));
                drag.sent = point;
            }
        })().catch(async (error) => {
            this.errors.set(drag.key, error instanceof Error ? error.message : String(error));
            if (this.active === drag) this.active = null;
            try { await drag.gesture.finish(); } catch {}
            this.changed();
            throw error;
        }).finally(() => { drag.pending = undefined; });
        return drag.pending;
    }
    finish(): Promise<void> {
        const drag = this.active;
        if (!drag) return Promise.resolve();
        if (drag.finishing) return drag.finishing;
        if (drag.timer !== undefined) this.timers.clear(drag.timer);
        drag.timer = undefined;
        drag.finishing = this.flush(drag).then(async () => {
            await drag.gesture.finish();
            const revision = drag.gesture.acceptedRevision?.();
            if (this.active === drag && revision !== undefined && this.readRevision < revision) {
                let entries = this.ending.get(drag.key);
                if (!entries) this.ending.set(drag.key, entries = new Map());
                entries.set(drag.desired.id, { point: { ...drag.desired }, revision, axes: drag.axes });
            }
        }).catch((error) => {
            this.errors.set(drag.key, error instanceof Error ? error.message : String(error));
        }).finally(() => { if (this.active === drag) this.active = null; this.changed(); });
        this.changed(); return drag.finishing;
    }
    abandon(): void {
        const drag = this.active;
        if (!drag) return;
        this.active = null;
        if (drag.timer !== undefined) this.timers.clear(drag.timer);
        // History navigation and removal discard unsent movement. Accepted
        // updates still close in order after the outstanding request.
        void (drag.pending ?? Promise.resolve()).catch(() => {}).then(() => drag.gesture.finish()).catch(() => {});
        this.changed();
    }
    disconnected(): void {
        if (this.active?.timer !== undefined) this.timers.clear(this.active.timer);
        this.active = null; this.curves.clear(); this.errors.clear(); this.ending.clear(); this.readRevision = 0; this.changed();
    }
    async add(target: NodeAddress, position: { seconds: number; value: number }): Promise<void> {
        const curve = this.curves.get(addressKey(target));
        if (!curve) return;
        const ids = new Set(curve.points.map((point) => point.id));
        let id = curve.points.reduce((max, point) => Math.max(max, point.id), -1) + 1;
        id %= 2 ** 32;
        while (ids.has(id)) id = (id + 1) % 2 ** 32;
        await this.send(target, AddPoint({ point: { id, ...position } }));
    }
    async remove(target: NodeAddress, id: number): Promise<void> { await this.send(target, RemovePoint({ id })); }
    private async send(target: NodeAddress, event: NodeEvent): Promise<void> {
        const key = addressKey(target); this.errors.delete(key);
        try { await this.transport.send(target, event); }
        catch (error) { this.errors.set(key, error instanceof Error ? error.message : String(error)); }
        this.changed();
    }
}

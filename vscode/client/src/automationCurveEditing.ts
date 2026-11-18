import { addressKey, type GraphScope, type GraphScopeNode, type NodeAddress, type Point as ScreenPoint } from "./graphScope";
import { AddPoint, MovePoint, RemovePoint, SetSegment } from "./generated/automationEvents";
import type { NodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";

import { automationCycleSeconds, type AutomationCurve, type AutomationPoint, type AutomationPointPosition, type CurveInterpolation } from "./automationCurveGeometry";
export { automationCurveBounds, automationCycleSeconds, automationWindow } from "./automationCurveGeometry";
export type { AutomationCurve, AutomationPoint, CurveInterpolation } from "./automationCurveGeometry";
export type CurveAxes = { left: number; top: number; width: number; height: number;
    begin: number; end: number; min: number; max: number };
type Transport = { send(target: NodeAddress, event: NodeEvent): Promise<void>; gesture(target: NodeAddress): NodeEventGesture };
type Timers = { set(callback: () => void): unknown; clear(handle: unknown): void };
type Drag = { key: string; target: NodeAddress; pointerId: number; axes: CurveAxes; desired: AutomationPointPosition;
    offset: { seconds: number; value: number };
    lastPosition?: ScreenPoint;
    sent: AutomationPointPosition | null; gesture: NodeEventGesture; timer?: unknown; pending?: Promise<void>;
    finishing?: Promise<void> };

export function readAutomationCurve(value: GraphScopeNode["construction"], sampleRate?: number | null): AutomationCurve | null {
    if (value?.definitionId !== "iv.builtin.automation.curve" || value.arguments.length !== 1) return null;
    const [points] = value.arguments;
    if (!Array.isArray(points)) return null;
    const ids = new Set<number>();
    const result: AutomationPoint[] = [];
    for (const point of points) {
        if (!point || !Number.isInteger(point.id) || point.id < 0 || point.id >= 2 ** 32 || ids.has(point.id)
            || typeof point.seconds !== "number" || !Number.isFinite(point.seconds)
            || typeof point.value !== "number" || !Number.isFinite(point.value) || !Number.isFinite(Math.fround(point.value))
            || !["empty", "hold", "bezier"].includes(point.interpolation)
            || typeof point.tension !== "number" || !Number.isFinite(point.tension) || point.tension < 0 || point.tension > 1) return null;
        const frames = point.seconds * (sampleRate ?? 0);
        if (!Number.isFinite(frames)) return null;
        const seconds = sampleRate && sampleRate > 0
            ? Math.sign(frames) * Math.floor(Math.abs(frames) + .5) / sampleRate : point.seconds;
        ids.add(point.id); result.push({ id: point.id, seconds, value: point.value,
            interpolation: point.interpolation, tension: point.tension });
    }
    // The point source breaks equal-position ties using persistent identity.
    result.sort((a, b) => a.seconds - b.seconds || a.id - b.id);
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
    private readonly ending = new Map<string, Map<number, { point: AutomationPointPosition; revision: number; axes: CurveAxes }>>();
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
    drafts(target: NodeAddress): AutomationPointPosition[] {
        const key = addressKey(target);
        const points = new Map<number, AutomationPointPosition>();
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
    activePoint(): AutomationPointPosition | null {
        return this.active?.finishing ? null : this.active?.desired ?? null;
    }
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
        this.active = { key, target: structuredClone(target), pointerId, axes: { ...axes }, desired: { id: initial.id, seconds: initial.seconds, value: initial.value },
            offset: { seconds: initial.seconds - cursor.seconds, value: initial.value - cursor.value },
            lastPosition: position ? { ...position } : undefined,
            sent: { id: initial.id, seconds: initial.seconds, value: initial.value }, gesture: this.transport.gesture(target) };
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
        await this.send(target, AddPoint({ point: { id, ...position, interpolation: "bezier", tension: 1 } }));
    }
    async segment(target: NodeAddress, id: number, interpolation: CurveInterpolation, tension: number): Promise<void> {
        await this.send(target, SetSegment({ id, interpolation, tension }));
    }
    async remove(target: NodeAddress, id: number): Promise<void> { await this.send(target, RemovePoint({ id })); }
    private async send(target: NodeAddress, event: NodeEvent): Promise<void> {
        const key = addressKey(target); this.errors.delete(key);
        try { await this.transport.send(target, event); }
        catch (error) { this.errors.set(key, error instanceof Error ? error.message : String(error)); }
        this.changed();
    }
}

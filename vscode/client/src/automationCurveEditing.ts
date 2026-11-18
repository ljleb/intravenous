import { addressKey, type GraphScope, type GraphScopeNode, type NodeAddress, type Point as ScreenPoint } from "./graphScope";
import { AddPoint, MovePoint, RemovePoint, SetBound, SetSegment } from "./generated/automationEvents";
import type { NodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { ScrubSensitivity } from "./scrubSensitivity";
import { updateSelection } from "./selection";
import { knobScrubSpeed } from "./knobPresentation";

import { automationBoundLimits, automationCycleSeconds, automationSampleIndex, automationUnitsPerPixel, automationValueAtFraction,
    automationValueGuides, clampAutomationValue, compareAutomationTime, defaultAutomationBounds, enabledAutomationBounds,
    type AutomationValueBounds, type AutomationValueAxis, type AutomationValueRuler, type AutomationCurve, type AutomationPoint, type AutomationPointPosition,
    type AutomationBound, type AutomationBoundSettings, type AutomationBoundSide, type CurveInterpolation } from "./automationCurveGeometry";
export { automationCycleSeconds, automationWindow } from "./automationCurveGeometry";
export type { AutomationCurve, AutomationPoint, CurveInterpolation } from "./automationCurveGeometry";
export type CurveAxes = { left: number; top: number; width: number; height: number;
    begin: number; end: number; spanSeconds?: number; ruler?: AutomationValueRuler } & AutomationValueAxis;
export type CurveDragKind = "point" | "tension";
type Transport = { send(target: NodeAddress, event: NodeEvent): Promise<void>; gesture(target: NodeAddress): NodeEventGesture };
type Timers = { set(callback: () => void): unknown; clear(handle: unknown): void };
type Drag = { key: string; target: NodeAddress; pointerId: number; axes: CurveAxes; valueBounds: AutomationValueBounds; desired: AutomationPointPosition;
    companions: readonly AutomationPointPosition[]; authoredBounds: AutomationValueBounds; anchorValue: number;
    kind: CurveDragKind; initial: AutomationPointPosition; grab?: ScreenPoint; sampleRate?: number;
    lastPosition?: ScreenPoint; sensitivity?: ScrubSensitivity;
    detents: readonly number[]; plateau: number; anchorPlateau: number;
    sent: AutomationPointPosition | null; gesture: NodeEventGesture; timer?: unknown; pending?: Promise<void>;
    finishing?: Promise<void> };
type BoundDrag = { key: string; target: NodeAddress; pointerId: number; side: AutomationBoundSide; settings: AutomationBoundSettings;
    limits: AutomationValueBounds; sensitivity: ScrubSensitivity; position: ScreenPoint; value: number;
    plateau: number; anchorPlateau: number };

function readBoundSettings(value: unknown): AutomationBoundSettings | null {
    if (!value || typeof value !== "object" || Array.isArray(value)) return null;
    const settings = value as Partial<Record<AutomationBoundSide, Partial<AutomationBound>>>;
    const read = (side: AutomationBoundSide) => {
        const bound = settings[side];
        if (bound !== undefined && (!bound || typeof bound !== "object" || Array.isArray(bound))) return null;
        const { value, enabled } = bound === undefined ? defaultAutomationBounds[side] : { value: 0, enabled: false, ...bound };
        return typeof value === "number" && Number.isFinite(Math.fround(value)) && typeof enabled === "boolean"
            ? { value: Math.fround(value), enabled } : null;
    };
    const min = read("min"), max = read("max");
    return min && max && !(min.enabled && max.enabled && min.value > max.value) ? { min, max } : null;
}

export function readAutomationCurve(value: GraphScopeNode["construction"], sampleRate?: number | null): AutomationCurve | null {
    if (value?.definitionId !== "iv.builtin.automation.curve" || value.arguments.length < 1 || value.arguments.length > 2) return null;
    const [points, configuration] = value.arguments;
    if (!Array.isArray(points)) return null;
    const bounds = value.arguments.length === 2 ? readBoundSettings(configuration) : undefined;
    if (bounds === null) return null;
    const ids = new Set<number>();
    const rate = sampleRate && Number.isSafeInteger(sampleRate) && sampleRate > 0 ? sampleRate : undefined;
    const result: AutomationPoint[] = [];
    for (const point of points) {
        if (!point || !Number.isInteger(point.id) || point.id < 0 || point.id >= 2 ** 32 || ids.has(point.id)
            || typeof point.seconds !== "number" || !Number.isFinite(point.seconds)
            || typeof point.value !== "number" || !Number.isFinite(point.value) || !Number.isFinite(Math.fround(point.value))
            || !["empty", "hold", "bezier"].includes(point.interpolation)
            || typeof point.tension !== "number" || !Number.isFinite(point.tension) || point.tension < 0 || point.tension > 1) return null;
        const text = point.sample_index;
        if (text !== undefined && (typeof text !== "string" || (text !== "" && (!rate || !/^-?\d{1,19}$/.test(text))))) return null;
        const sampleIndex = text ? BigInt(text) : rate ? automationSampleIndex(point.seconds, rate) : undefined;
        if (sampleIndex !== undefined && sampleIndex !== BigInt.asIntN(64, sampleIndex)) return null;
        const seconds = rate && sampleIndex !== undefined ? Number(sampleIndex) / rate : point.seconds;
        ids.add(point.id); result.push({ id: point.id, seconds, value: point.value,
            interpolation: point.interpolation, tension: point.tension, ...(sampleIndex !== undefined ? { sampleIndex } : {}) });
    }
    // The point source breaks equal-position ties using persistent identity.
    result.sort((a, b) => compareAutomationTime(a, b) || a.id - b.id);
    return { points: result, cycleSeconds: automationCycleSeconds(rate), sampleRate: rate, ...(bounds ? { bounds } : {}) };
}

export function curvePointAt(axes: CurveAxes, position: ScreenPoint, sampleRate?: number) {
    const offset = (position.x - axes.left) / axes.width * (axes.spanSeconds ?? axes.end - axes.begin);
    // Creation needs an absolute value. At an unbounded edge use the nearest
    // pixel centre, since the mathematical edge has no finite inverse.
    const halfPixel = Math.min(.5, .5 / axes.height);
    const lower = axes.min === undefined ? halfPixel : 0;
    const upper = axes.max === undefined ? 1 - halfPixel : 1;
    const fraction = Math.max(lower, Math.min(upper, 1 - (position.y - axes.top) / axes.height));
    return { seconds: axes.begin + offset, sampleIndex: sampleRate ? automationSampleIndex(axes.begin, sampleRate, offset) : undefined,
        value: automationValueAtFraction(fraction, axes) };
}

// A webview owns the accepted read model and one local point draft. Components
// can remount while the pointer and the coalesced RPC remain owned here.
export class AutomationCurveEditing {
    private readonly curves = new Map<string, AutomationCurve>();
    private readonly errors = new Map<string, string>();
    private readonly ending = new Map<string, Map<number, { point: AutomationPointPosition; revision: number; axes: CurveAxes }>>();
    private readRevision = 0;
    private active: Drag | null = null;
    private boundDrag: BoundDrag | null = null;
    private readonly pendingBounds = new Map<string, { settings: AutomationBoundSettings; revision?: number }>();
    private readonly selection = new Map<string, ReadonlySet<number>>();
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
        for (const [key, pending] of this.pendingBounds) {
            if (!next.has(key) || (pending.revision !== undefined && this.readRevision >= pending.revision)) this.pendingBounds.delete(key);
        }
        if (this.boundDrag && !next.has(this.boundDrag.key)) this.boundDrag = null;
        for (const [key, ids] of this.selection) {
            const kept = new Set(next.get(key)?.points.filter((point) => ids.has(point.id)).map((point) => point.id));
            if (kept.size) this.selection.set(key, kept);
            else this.selection.delete(key);
        }
        if (this.active && (!next.has(this.active.key)
            || !next.get(this.active.key)!.points.some((point) => point.id === this.active!.desired.id))) {
            this.abandon();
        }
        this.changed();
    }
    view(target: NodeAddress): AutomationCurve | null {
        return this.curves.get(addressKey(target)) ?? null;
    }
    bounds(target: NodeAddress): AutomationBoundSettings | undefined {
        const key = addressKey(target);
        const drag = this.boundDrag;
        if (drag?.key === key)
            return { ...drag.settings, [drag.side]: { enabled: true, value: drag.value } };
        return this.pendingBounds.get(key)?.settings ?? this.curves.get(key)?.bounds;
    }
    boundsPending(target: NodeAddress): boolean { return this.pendingBounds.has(addressKey(target)); }
    activeBound(): { side: AutomationBoundSide; value: number } | null {
        return this.boundDrag ? { side: this.boundDrag.side, value: this.boundDrag.value } : null;
    }
    drafts(target: NodeAddress): AutomationPointPosition[] {
        const key = addressKey(target);
        const points = new Map<number, AutomationPointPosition>();
        for (const entry of this.ending.get(key)?.values() ?? []) points.set(entry.point.id, entry.point);
        if (this.active?.key === key) for (const point of this.movedPoints(this.active, this.active.desired)) points.set(point.id, point);
        return [...points.values()];
    }
    error(target: NodeAddress): string { return this.errors.get(addressKey(target)) ?? ""; }
    selectedPoints(target: NodeAddress): ReadonlySet<number> {
        return this.selection.get(addressKey(target)) ?? new Set();
    }
    selectPoints(target: NodeAddress, ids: Iterable<number>, modifiers?: Parameters<typeof updateSelection>[2]): void {
        const key = addressKey(target);
        const selected = updateSelection(this.selectedPoints(target), ids, modifiers);
        if (selected.size) this.selection.set(key, selected);
        else this.selection.delete(key);
        this.changed();
    }
    axes(target: NodeAddress): CurveAxes | null {
        const key = addressKey(target);
        return this.active?.key === key ? this.active.axes : this.ending.get(key)?.values().next().value?.axes ?? null;
    }
    pointerId(): number | null {
        return this.boundDrag?.pointerId ?? (this.active?.finishing ? null : this.active?.pointerId ?? null);
    }
    activePoint(): AutomationPointPosition | null {
        return this.active?.finishing ? null : this.active?.desired ?? null;
    }
    sensitivityHint(): string | undefined {
        if (this.boundDrag) return this.boundDrag.sensitivity.hint();
        return this.active?.finishing ? undefined : this.active?.sensitivity?.hint();
    }
    changeSensitivity(delta: number): boolean {
        const bound = this.boundDrag;
        if (bound) {
            if (bound.sensitivity.change(delta, -bound.position.y, bound.value)) {
                bound.anchorPlateau = bound.plateau;
                this.changed();
            }
            return true;
        }
        const drag = this.active;
        if (!drag?.sensitivity || !drag.lastPosition || drag.finishing) return false;
        if (drag.sensitivity.change(delta, -drag.lastPosition.y, clampAutomationValue(drag.desired.value, drag.valueBounds))) {
            drag.anchorPlateau = drag.plateau;
            this.changed();
        }
        return true;
    }
    begin(target: NodeAddress, id: number, pointerId: number, axes: CurveAxes, position?: ScreenPoint,
        kind: CurveDragKind = "point"): boolean {
        if (this.active || this.boundDrag || this.boundsPending(target) || axes.width <= 0 || axes.height <= 0) return false;
        const key = addressKey(target);
        const curve = this.curves.get(key);
        const point = curve?.points.find((entry) => entry.id === id);
        if (!curve || !point || (kind === "point" && !curve.sampleRate) || (kind === "tension" && (point.interpolation !== "bezier" || !position))) return false;
        // A re-grab starts from the value still displayed while its accepted
        // read-model update is in transit, rather than jumping to an old read.
        const visible = this.ending.get(key)?.get(id)?.point ?? point;
        const initial: AutomationPointPosition = { id, seconds: visible.seconds, value: visible.value, sampleIndex: visible.sampleIndex,
            ...(kind === "tension" || visible !== point && visible.tension !== undefined ? { tension: visible.tension ?? point.tension } : {}) };
        this.errors.delete(key);
        const authoredBounds = enabledAutomationBounds(curve.bounds);
        const anchorValue = clampAutomationValue(initial.value, authoredBounds);
        const selected = this.selectedPoints(target);
        const companions = kind === "point" && selected.size > 1 && selected.has(id)
            ? curve.points.filter((entry) => entry.id !== id && selected.has(entry.id))
                .map((entry) => this.ending.get(key)?.get(entry.id)?.point ?? entry) : [];
        const valueBounds = { ...authoredBounds };
        for (const companion of companions) {
            const offset = clampAutomationValue(companion.value, authoredBounds) - anchorValue;
            if (authoredBounds.min !== undefined) valueBounds.min = Math.max(valueBounds.min!, authoredBounds.min - offset);
            if (authoredBounds.max !== undefined) valueBounds.max = Math.min(valueBounds.max!, authoredBounds.max - offset);
        }
        const detents = kind === "point" && position ? automationValueGuides(axes, axes.height, axes.ruler)
            .map(({ value }) => value).filter((value) => value > (valueBounds.min ?? -Infinity) && value < (valueBounds.max ?? Infinity)) : [];
        this.active = { key, target: structuredClone(target), pointerId, axes: { ...axes }, desired: initial, initial,
            kind, valueBounds, authoredBounds, anchorValue, companions, sampleRate: curve.sampleRate, grab: position ? { ...position } : undefined,
            lastPosition: position ? { ...position } : undefined,
            sensitivity: kind === "point" && position
                ? new ScrubSensitivity(automationUnitsPerPixel(axes, axes.height), -position.y, clampAutomationValue(initial.value, valueBounds)) : undefined,
            detents,
            plateau: 0, anchorPlateau: 0,
            sent: initial, gesture: this.transport.gesture(target) };
        this.changed(); return true;
    }
    beginBound(target: NodeAddress, side: AutomationBoundSide, pointerId: number, position: ScreenPoint, precise: boolean): boolean {
        const key = addressKey(target);
        const settings = this.curves.get(key)?.bounds ?? defaultAutomationBounds;
        if (this.active || this.boundDrag || this.pendingBounds.has(key) || !settings[side].enabled) return false;
        const limits = automationBoundLimits(settings, side);
        const value = settings[side].value;
        const speed = knobScrubSpeed({ ...limits, defaultValue: defaultAutomationBounds[side].value });
        this.errors.delete(key);
        this.boundDrag = { key, target: structuredClone(target), pointerId, side, settings, limits, position: { ...position }, value,
            sensitivity: new ScrubSensitivity(speed, -position.y, value, precise), plateau: 0, anchorPlateau: 0 };
        this.changed();
        return true;
    }
    moveBy(pointerId: number, delta: ScreenPoint): void {
        const position = this.boundDrag?.position ?? this.active?.lastPosition;
        if (position) this.move(pointerId, { x: position.x + delta.x, y: position.y + delta.y });
    }
    move(pointerId: number, position: ScreenPoint): void {
        if (this.boundDrag) {
            if (this.boundDrag.pointerId === pointerId) this.moveBound(this.boundDrag, position);
            return;
        }
        const drag = this.active;
        if (!drag || drag.finishing || drag.pointerId !== pointerId) return;
        const previous = drag.lastPosition;
        if (previous?.x === position.x && previous.y === position.y) return;
        drag.lastPosition = { ...position };
        const { initial, grab, axes, sampleRate } = drag;
        if (drag.kind === "tension") {
            const tension = Math.fround(Math.min(1, Math.max(0, initial.tension! + (position.x - grab!.x) / 100)));
            if (tension === drag.desired.tension) return;
            drag.desired = { ...initial, tension };
        } else {
            if (!sampleRate) return;
            let index: bigint | undefined;
            let value: number;
            if (grab && drag.sensitivity) {
                const delta = automationSampleIndex((position.x - grab.x) / axes.width
                    * (axes.spanSeconds ?? axes.end - axes.begin), sampleRate);
                if (delta === undefined || initial.sampleIndex === undefined) return;
                index = initial.sampleIndex + delta;
                const vertical = -position.y;
                const { sensitivity, valueBounds } = drag;
                if (position.y === previous?.y) {
                    value = clampAutomationValue(drag.desired.value, valueBounds);
                } else {
                    const snapped = sensitivity.valueAtDetents(vertical, drag.detents, drag.anchorPlateau);
                    value = clampAutomationValue(snapped.value, valueBounds);
                    drag.plateau = snapped.plateau;
                    if (snapped.value !== value) {
                        // Discard travel beyond the limit so reversing responds immediately.
                        sensitivity.anchorAt(vertical, value);
                        drag.plateau = drag.anchorPlateau = 0;
                    }
                }
            } else {
                const next = curvePointAt(axes, position, sampleRate);
                index = next.sampleIndex;
                value = clampAutomationValue(next.value, drag.valueBounds);
            }
            if (index === undefined) return;
            const sampleIndex = BigInt.asIntN(64, index);
            const authoredValue = Math.fround(value);
            if (!Number.isFinite(authoredValue) || (sampleIndex === drag.desired.sampleIndex && authoredValue === drag.desired.value)) return;
            drag.desired = { ...initial, sampleIndex, seconds: Number(sampleIndex) / sampleRate, value: authoredValue };
        }
        if (!drag.companions.length && drag.timer === undefined && !drag.pending) drag.timer = this.timers.set(() => {
            drag.timer = undefined;
            void this.flush(drag).catch(() => {});
        });
        this.changed();
    }
    private moveBound(drag: BoundDrag, position: ScreenPoint): void {
        drag.position = { ...position };
        const vertical = -position.y;
        const { sensitivity, limits } = drag;
        const defaultValue = defaultAutomationBounds[drag.side].value;
        let raw = sensitivity.valueAt(vertical);
        if (clampAutomationValue(defaultValue, limits) === defaultValue) {
            const snapped = sensitivity.valueAtDetent(defaultValue, vertical - sensitivity.anchorPosition, drag.anchorPlateau);
            raw = snapped.value;
            drag.plateau = snapped.plateau;
        }
        const clamped = clampAutomationValue(raw, limits);
        const value = Math.fround(clamped);
        if (!Number.isFinite(value)) return;
        if (raw !== clamped) {
            sensitivity.anchorAt(vertical, value);
            drag.anchorPlateau = drag.plateau;
        }
        if (value !== drag.value) { drag.value = value; this.changed(); }
    }
    async setBound(target: NodeAddress, side: AutomationBoundSide, bound: AutomationBound): Promise<void> {
        const key = addressKey(target);
        const curve = this.curves.get(key);
        if (!curve || this.active || this.boundDrag || this.pendingBounds.has(key)) return;
        const settings = curve.bounds ?? defaultAutomationBounds;
        const value = Math.fround(bound.enabled ? clampAutomationValue(bound.value, automationBoundLimits(settings, side)) : bound.value);
        if (!Number.isFinite(value) || (value === settings[side].value && bound.enabled === settings[side].enabled)) return;
        const edited = { enabled: bound.enabled, value };
        const pending: { settings: AutomationBoundSettings; revision?: number } = { settings: { ...settings, [side]: edited } };
        const gesture = this.transport.gesture(target);
        this.pendingBounds.set(key, pending);
        this.errors.delete(key);
        this.changed();
        try {
            try { await gesture.send(SetBound({ side, bound: edited })); }
            finally { await gesture.finish(); }
            pending.revision = gesture.acceptedRevision?.();
        } catch (error) {
            if (this.pendingBounds.get(key) === pending) this.errors.set(key, error instanceof Error ? error.message : String(error));
        } finally {
            if (this.pendingBounds.get(key) === pending && (pending.revision === undefined || this.readRevision >= pending.revision))
                this.pendingBounds.delete(key);
            this.changed();
        }
    }
    private movedPoints(drag: Drag, anchor: AutomationPointPosition): AutomationPointPosition[] {
        if (!drag.companions.length) return [anchor];
        const timeDelta = anchor.sampleIndex! - drag.initial.sampleIndex!;
        const valueDelta = anchor.value - drag.anchorValue;
        return [anchor, ...drag.companions.map((point) => {
            if (anchor === drag.initial) return point;
            const sampleIndex = BigInt.asIntN(64, point.sampleIndex! + timeDelta);
            const initial = clampAutomationValue(point.value, drag.authoredBounds);
            const value = Math.fround(clampAutomationValue(initial + valueDelta, drag.authoredBounds));
            return { ...point, sampleIndex, seconds: Number(sampleIndex) / drag.sampleRate!, value };
        })];
    }
    private flush(drag: Drag): Promise<void> {
        if (drag.pending) return drag.pending;
        drag.pending = (async () => {
            while (this.active === drag && (drag.sent?.sampleIndex !== drag.desired.sampleIndex
                || drag.sent?.value !== drag.desired.value || drag.sent?.tension !== drag.desired.tension)) {
                const point = { ...drag.desired };
                if (drag.kind === "tension") {
                    await drag.gesture.send(SetSegment({ id: point.id, interpolation: "bezier", tension: point.tension! }));
                } else {
                    for (const moved of this.movedPoints(drag, point)) {
                        if (this.active !== drag) break;
                        await drag.gesture.send(MovePoint({ id: moved.id, seconds: 0,
                            sample_index: moved.sampleIndex!.toString(), value: moved.value }));
                    }
                }
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
        const bound = this.boundDrag;
        if (bound) {
            this.boundDrag = null;
            const saved = this.setBound(bound.target, bound.side, { enabled: true, value: bound.value });
            this.changed();
            return saved;
        }
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
                for (const point of this.movedPoints(drag, drag.desired))
                    entries.set(point.id, { point, revision, axes: drag.axes });
            }
        }).catch((error) => {
            this.errors.set(drag.key, error instanceof Error ? error.message : String(error));
        }).finally(() => { if (this.active === drag) this.active = null; this.changed(); });
        this.changed(); return drag.finishing;
    }
    abandon(): void {
        if (this.boundDrag) {
            this.boundDrag = null;
            this.changed();
            return;
        }
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
        this.boundDrag = null; this.pendingBounds.clear();
        this.active = null; this.selection.clear(); this.curves.clear(); this.errors.clear(); this.ending.clear(); this.readRevision = 0; this.changed();
    }
    async add(target: NodeAddress, position: { seconds: number; value: number; sampleIndex?: bigint }): Promise<void> {
        const curve = this.curves.get(addressKey(target));
        if (!curve?.sampleRate || this.boundsPending(target)) return;
        const sampleIndex = position.sampleIndex ?? automationSampleIndex(position.seconds, curve.sampleRate);
        if (sampleIndex === undefined || !Number.isFinite(Math.fround(position.value))) return;
        const value = Math.fround(clampAutomationValue(position.value, enabledAutomationBounds(curve.bounds)));
        const ids = new Set(curve.points.map((point) => point.id));
        let id = curve.points.reduce((max, point) => Math.max(max, point.id), -1) + 1;
        id %= 2 ** 32;
        while (ids.has(id)) id = (id + 1) % 2 ** 32;
        await this.send(target, AddPoint({ point: { id, seconds: 0, sample_index: BigInt.asIntN(64, sampleIndex).toString(),
            value, interpolation: "bezier", tension: 1 } }));
    }
    async segment(target: NodeAddress, id: number, interpolation: CurveInterpolation, tension: number): Promise<void> {
        await this.send(target, SetSegment({ id, interpolation, tension }));
    }
    async remove(target: NodeAddress, id: number): Promise<void> { await this.send(target, RemovePoint({ id })); }
    async removeSelection(target: NodeAddress): Promise<void> {
        const ids = this.selectedPoints(target);
        if (!ids.size || this.active) return;
        const gesture = this.transport.gesture(target);
        const key = addressKey(target);
        this.errors.delete(key);
        try {
            for (const id of ids) await gesture.send(RemovePoint({ id }));
        } catch (error) {
            this.errors.set(key, error instanceof Error ? error.message : String(error));
        } finally {
            await gesture.finish().catch((error) => {
                this.errors.set(key, error instanceof Error ? error.message : String(error));
            });
            this.changed();
        }
    }
    private async send(target: NodeAddress, event: NodeEvent): Promise<void> {
        const key = addressKey(target); this.errors.delete(key);
        try { await this.transport.send(target, event); }
        catch (error) { this.errors.set(key, error instanceof Error ? error.message : String(error)); }
        this.changed();
    }
}

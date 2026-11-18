import { addressKey, type GraphScope, type GraphScopeNode, type NodeAddress, type Point as ScreenPoint } from "./graphScope";
import { MovePoint, PlacePoint, RemovePoint, SetSegment, type SetSegmentPayload } from "./generated/automationEvents";
import type { NodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { ScrubSensitivity } from "./scrubSensitivity";
import { automationTimeDrag, initialGapAttachment, placeAutomationPoint, pointInsertionIndex,
    type GapAttachment, type PointTime } from "./automationPointPlacement";
import { updateSelection } from "./selection";
import { knobScrubSpeed } from "./knobPresentation";

import { automationCycleSeconds, automationSampleIndex, automationUnitsPerPixel, automationValueAtFraction,
    automationValueGuides, clampAutomationValue, compareAutomationPoints,
    type AutomationValueAxis, type AutomationValueRuler, type AutomationCurve, type AutomationPoint, type AutomationPointPosition,
    type CurveInterpolation } from "./automationCurveGeometry";
export { automationCycleSeconds, automationWindow } from "./automationCurveGeometry";
export type { AutomationCurve, AutomationPoint, CurveInterpolation } from "./automationCurveGeometry";
export type CurveAxes = { left: number; top: number; width: number; height: number;
    begin: number; end: number; spanSeconds?: number; ruler?: AutomationValueRuler } & AutomationValueAxis;
export type CurveDragKind = "point" | "tension" | "tensionKnob";
export const automationTensionLimits = { min: 0, max: 1, defaultValue: 1 } as const;
type Transport = { send(target: NodeAddress, event: NodeEvent): Promise<void>; gesture(target: NodeAddress): NodeEventGesture };
type Timers = { set(callback: () => void): unknown; clear(handle: unknown): void };
type TimeLimits = { min?: bigint; max?: bigint };
type PointDrag = {
    neighbors: AutomationPoint[]; byId: ReadonlyMap<number, AutomationPoint>; base: AutomationPoint;
    adding: boolean; appeared: boolean; placing: boolean; deleteOnClick: boolean; moved: boolean;
    initialSlot: number; gapAttachment: GapAttachment;
    timeAt?: (pixels: number) => PointTime; time?: PointTime; sentOrder: readonly number[]; limits: TimeLimits;
    segments: ReadonlyMap<number, SetSegmentPayload>; sentSegments: ReadonlyMap<number, SetSegmentPayload>;
    baseSegments: ReadonlyMap<number, SetSegmentPayload>; touched: Set<number>;
};
type Drag = { key: string; target: NodeAddress; pointerId: number; axes: CurveAxes; desired: AutomationPoint;
    companions: readonly AutomationPointPosition[];
    kind: CurveDragKind; initial: AutomationPoint; grab?: ScreenPoint; sampleRate?: number;
    lastPosition?: ScreenPoint; sensitivity?: ScrubSensitivity;
    detents: readonly number[]; plateau: number; anchorPlateau: number; snappedValue?: number;
    pointDrag?: PointDrag;
    sent: AutomationPoint | null; gesture: NodeEventGesture; timer?: unknown; pending?: Promise<void>;
    finishing?: Promise<void> };
export function readAutomationCurve(value: GraphScopeNode["construction"], sampleRate?: number | null): AutomationCurve | null {
    if (value?.definitionId !== "iv.builtin.automation.curve" || value.arguments.length !== 1) return null;
    const [points] = value.arguments;
    if (!Array.isArray(points)) return null;
    const ids = new Set<number>();
    const rate = sampleRate && Number.isSafeInteger(sampleRate) && sampleRate > 0 ? sampleRate : undefined;
    const result: AutomationPoint[] = [];
    for (const point of points) {
        if (!point || !Number.isInteger(point.id) || point.id < 0 || point.id >= 2 ** 32 || ids.has(point.id)
            || typeof point.seconds !== "number" || !Number.isFinite(point.seconds)
            || typeof point.value !== "number" || !Number.isFinite(point.value) || !Number.isFinite(Math.fround(point.value))
            || !["empty", "hold", "bezier"].includes(point.interpolation)
            || typeof point.tension !== "number" || !Number.isFinite(point.tension) || point.tension < 0 || point.tension > 1) return null;
        const order = point.order;
        if (order !== undefined && (!Number.isInteger(order) || order < 0 || order >= 2 ** 32)) return null;
        const text = point.sample_index;
        if (text !== undefined && (typeof text !== "string" || (text !== "" && (!rate || !/^-?\d{1,19}$/.test(text))))) return null;
        const sampleIndex = text ? BigInt(text) : rate ? automationSampleIndex(point.seconds, rate) : undefined;
        if (sampleIndex !== undefined && sampleIndex !== BigInt.asIntN(64, sampleIndex)) return null;
        const seconds = rate && sampleIndex !== undefined ? Number(sampleIndex) / rate : point.seconds;
        ids.add(point.id); result.push({ id: point.id, seconds, value: point.value,
            interpolation: point.interpolation, tension: point.tension, ...(sampleIndex !== undefined ? { sampleIndex } : {}),
            ...(order !== undefined ? { order } : {}) });
    }
    result.sort(compareAutomationPoints);
    return { points: result, cycleSeconds: automationCycleSeconds(rate), sampleRate: rate };
}

export function curvePointAt(axes: CurveAxes, position: ScreenPoint, sampleRate?: number) {
    const offset = (position.x - axes.left) / axes.width * (axes.spanSeconds ?? axes.end - axes.begin);
    // At the edge of the curved view, use the nearest pixel center so that
    // the inverse arctangent scale stays finite.
    const inset = axes.display === "cropped" ? 0 : Math.min(.5, .5 / axes.height);
    const fraction = Math.max(inset, Math.min(1 - inset, 1 - (position.y - axes.top) / axes.height));
    return { seconds: axes.begin + offset, sampleIndex: sampleRate ? automationSampleIndex(axes.begin, sampleRate, offset) : undefined,
        value: automationValueAtFraction(fraction, axes) };
}

const forwardSampleDistance = (from: AutomationPointPosition, to: AutomationPointPosition) =>
    BigInt.asUintN(64, to.sampleIndex! - from.sampleIndex!);

function sampleTravelLimit(from: AutomationPointPosition, to: AutomationPointPosition, wraps: boolean): bigint {
    const distance = forwardSampleDistance(from, to);
    const order = (from.order ?? 0) - (to.order ?? 0) || from.id - to.id;
    return (wraps && distance === 0n ? 1n << 64n : distance) - (order > 0 ? 1n : 0n);
}

function orderedTimeLimits(points: readonly AutomationPoint[], moving: ReadonlySet<number>): TimeLimits {
    const limits: TimeLimits = {};
    for (let i = 0; i < points.length; ++i) {
        const point = points[i];
        if (!moving.has(point.id)) continue;
        const before = points[(i + points.length - 1) % points.length], after = points[(i + 1) % points.length];
        if (!moving.has(before.id)) {
            const min = -sampleTravelLimit(before, point, i === 0);
            if (limits.min === undefined || min > limits.min) limits.min = min;
        }
        if (!moving.has(after.id)) {
            const max = sampleTravelLimit(point, after, i === points.length - 1);
            if (limits.max === undefined || max < limits.max) limits.max = max;
        }
    }
    return limits;
}

const segmentEdit = ({ id, interpolation, tension }: AutomationPoint): SetSegmentPayload => ({ id, interpolation, tension });
const sameSegment = (left?: SetSegmentPayload, right?: SetSegmentPayload) =>
    left?.id === right?.id && left?.interpolation === right?.interpolation && left?.tension === right?.tension;
const sameOrder = (left: readonly number[], right?: readonly number[]) =>
    left.length === (right?.length ?? 0) && left.every((id, rank) => id === right![rank]);

function segmentChanges(drag: PointDrag): SetSegmentPayload[] {
    const edits: SetSegmentPayload[] = [];
    for (const [id] of drag.sentSegments) {
        if (!drag.segments.has(id)) edits.push(segmentEdit(drag.byId.get(id)!));
    }
    for (const [id, segment] of drag.segments) {
        if (!sameSegment(drag.sentSegments.get(id), segment)) edits.push(segment);
    }
    return edits;
}

function nextPointId(points: readonly AutomationPointPosition[]): number {
    const ids = new Set(points.map((point) => point.id));
    let id = (points.reduce((max, point) => Math.max(max, point.id), -1) + 1) % 2 ** 32;
    while (ids.has(id)) id = (id + 1) % 2 ** 32;
    return id;
}

// A webview owns the accepted read model and one local point draft. Components
// can remount while the pointer and the coalesced RPC remain owned here.
export class AutomationCurveEditing {
    private readonly curves = new Map<string, AutomationCurve>();
    private readonly errors = new Map<string, string>();
    private readonly ending = new Map<string, Map<number, { point: AutomationPointPosition; revision: number; axes: CurveAxes }>>();
    private readRevision = 0;
    private active: Drag | null = null;
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
        for (const [key, ids] of this.selection) {
            const kept = new Set(next.get(key)?.points.filter((point) => ids.has(point.id)).map((point) => point.id));
            if (kept.size) this.selection.set(key, kept);
            else this.selection.delete(key);
        }
        const active = this.active;
        if (active && !active.finishing) {
            const curve = next.get(active.key);
            const present = curve?.points.some((point) => point.id === active.desired.id);
            if (!curve || (!present && active.pointDrag?.appeared !== false)) this.abandon();
            else if (present && active.pointDrag) active.pointDrag.appeared = true;
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
        if (this.active?.key === key) for (const point of this.draftPoints(this.active)) points.set(point.id, point);
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
        return this.active?.finishing ? null : this.active?.pointerId ?? null;
    }
    activePoint(): AutomationPointPosition | null {
        return this.active?.finishing ? null : this.active?.desired ?? null;
    }
    activeSnap(target: NodeAddress): { point: AutomationPointPosition; value: number } | null {
        const drag = this.active;
        return drag && !drag.finishing && drag.key === addressKey(target) && drag.snappedValue !== undefined
            ? { point: drag.desired, value: drag.snappedValue } : null;
    }
    sensitivityHint(): string | undefined {
        return this.active?.finishing ? undefined : this.active?.sensitivity?.hint();
    }
    changeSensitivity(delta: number): boolean {
        const drag = this.active;
        if (!drag?.sensitivity || !drag.lastPosition || drag.finishing) return false;
        const value = drag.kind === "tensionKnob" ? drag.desired.tension : drag.desired.value;
        if (drag.sensitivity.change(delta, -drag.lastPosition.y, value)) {
            drag.anchorPlateau = drag.plateau;
            this.changed();
        }
        return true;
    }
    begin(target: NodeAddress, id: number | null, pointerId: number, axes: CurveAxes, position?: ScreenPoint,
        kind: CurveDragKind = "point", shift = false, precise = false): boolean {
        if (this.active || axes.width <= 0 || axes.height <= 0) return false;
        const key = addressKey(target);
        const curve = this.curves.get(key);
        if (!curve || (kind === "point" && !curve.sampleRate)) return false;
        // Re-grabs and neighbor searches use the same points shown by the lane.
        const pending = this.ending.get(key);
        const visiblePoints = new Map(curve.points.map((point) => [point.id, point]));
        for (const { point } of pending?.values() ?? []) {
            const accepted = visiblePoints.get(point.id);
            if (accepted) visiblePoints.set(point.id, { ...accepted, ...point,
                interpolation: point.interpolation ?? accepted.interpolation, tension: point.tension ?? accepted.tension });
            else if (point.interpolation !== undefined && point.tension !== undefined)
                visiblePoints.set(point.id, { ...point, interpolation: point.interpolation, tension: point.tension });
        }
        const points = [...visiblePoints.values()].sort(compareAutomationPoints);
        const adding = id === null;
        let visible = id === null ? undefined : visiblePoints.get(id);
        if (adding && kind === "point" && position) {
            const at = curvePointAt(axes, position, curve.sampleRate);
            const value = Math.fround(at.value);
            if (at.sampleIndex === undefined || !Number.isFinite(value)) return false;
            visible = { ...at, id: nextPointId(points), sampleIndex: BigInt.asIntN(64, at.sampleIndex),
                value, interpolation: "empty", tension: 1 };
        }
        if (!visible || (kind === "point" && visible.sampleIndex === undefined)
            || (kind !== "point" && (visible.interpolation !== "bezier" || !position))) return false;
        const initial = visible;
        this.errors.delete(key);
        const selected = this.selectedPoints(target);
        const companions = !adding && kind === "point" && selected.size > 1 && selected.has(initial.id)
            ? points.filter((entry) => entry.id !== initial.id && selected.has(entry.id)).map((entry) =>
                pending?.get(entry.id)?.point ?? entry) : [];
        const detents = kind === "point" && position ? automationValueGuides(axes, axes.height, axes.ruler)
            .map(({ value }) => value) : [];
        const neighbors = kind === "point" ? points.filter((point) => point.id !== initial.id) : [];
        const initialSlot = pointInsertionIndex(neighbors, initial);
        const gapAttachment = adding ? "nearest" : initialGapAttachment(neighbors, initial, initialSlot);
        const placed = adding ? placeAutomationPoint(neighbors, null, initial, initialSlot, gapAttachment) : null;
        const desired = placed?.point ?? initial;
        const secondsPerPixel = (axes.spanSeconds ?? axes.end - axes.begin) / axes.width;
        const pointDrag: PointDrag | undefined = kind === "point" ? {
            neighbors, byId: visiblePoints, base: desired, adding, appeared: !adding && curve.points.some(({ id }) => id === initial.id),
            placing: !shift && !companions.length, deleteOnClick: !adding && shift, moved: false,
            initialSlot, gapAttachment,
            timeAt: position && !companions.length ? automationTimeDrag(neighbors, initial, secondsPerPixel, curve.sampleRate!) : undefined,
            sentOrder: [],
            limits: companions.length ? orderedTimeLimits(points, new Set([initial.id, ...companions.map((point) => point.id)])) : {},
            segments: placed?.segments ?? new Map(), sentSegments: new Map(), baseSegments: placed?.segments ?? new Map(),
            touched: new Set(placed?.segments.keys()),
        } : undefined;
        const sensitivity = !position ? undefined : kind === "point"
            ? new ScrubSensitivity(automationUnitsPerPixel(axes, axes.height), -position.y, initial.value)
            : kind === "tensionKnob"
                ? new ScrubSensitivity(knobScrubSpeed(automationTensionLimits), -position.y, initial.tension, precise) : undefined;
        const drag: Drag = { key, target: structuredClone(target), pointerId, axes: { ...axes }, desired, initial,
            kind, companions, sampleRate: curve.sampleRate, grab: position ? { ...position } : undefined,
            lastPosition: position ? { ...position } : undefined,
            sensitivity, detents, pointDrag,
            plateau: 0, anchorPlateau: 0,
            sent: adding ? null : initial, gesture: this.transport.gesture(target) };
        this.active = drag;
        if (adding) void this.flush(drag).catch(() => {});
        this.changed(); return true;
    }
    setPreservePointType(shift: boolean): void {
        const drag = this.active;
        const point = drag?.pointDrag;
        if (!drag || drag.finishing || !point) return;
        if (!shift) point.deleteOnClick = false;
        const placing = !shift;
        if (drag.companions.length || point.placing === placing) return;
        point.placing = placing;
        if (!point.time) return;
        this.applyPlacement(drag, drag.desired, point.time);
        this.scheduleDrag(drag);
    }
    moveBy(pointerId: number, delta: ScreenPoint): void {
        const position = this.active?.lastPosition;
        if (position) this.move(pointerId, { x: position.x + delta.x, y: position.y + delta.y });
    }
    move(pointerId: number, position: ScreenPoint): void {
        const drag = this.active;
        if (!drag || drag.finishing || drag.pointerId !== pointerId) return;
        const previous = drag.lastPosition;
        if (previous?.x === position.x && previous.y === position.y) return;
        drag.lastPosition = { ...position };
        const { initial, grab, axes, sampleRate } = drag;
        if (drag.kind !== "point") {
            const { sensitivity } = drag;
            const raw = sensitivity ? sensitivity.valueAt(-position.y) : initial.tension + (position.x - grab!.x) / 100;
            const clamped = clampAutomationValue(raw, automationTensionLimits);
            if (clamped !== raw) sensitivity?.anchorAt(-position.y, clamped);
            const tension = Math.fround(clamped);
            if (tension === drag.desired.tension) return;
            drag.desired = { ...initial, tension };
        } else {
            if (!sampleRate) return;
            const pointDrag = drag.pointDrag!;
            if (pointDrag.deleteOnClick && !pointDrag.moved && grab
                && Math.hypot(position.x - grab.x, position.y - grab.y) <= 5) return;
            pointDrag.moved = true;
            let time = pointDrag.time;
            let index = drag.desired.sampleIndex;
            let value: number;
            const previousSnap = drag.snappedValue;
            if (grab && drag.sensitivity) {
                if (position.x !== previous?.x || (!pointDrag.time && position.x !== grab.x)) {
                    const pixels = position.x - grab.x;
                    if (pointDrag.timeAt) {
                        time = pointDrag.timeAt(pixels);
                        index = time.sampleIndex;
                    } else {
                        const delta = automationSampleIndex(pixels / axes.width * (axes.spanSeconds ?? axes.end - axes.begin), sampleRate);
                        if (delta === undefined) return;
                        index = initial.sampleIndex! + delta;
                    }
                }
                const vertical = -position.y;
                const { sensitivity } = drag;
                if (position.y === previous?.y) {
                    value = drag.desired.value;
                } else {
                    const snapped = sensitivity.valueAtDetents(vertical, drag.detents, drag.anchorPlateau);
                    value = snapped.value;
                    drag.plateau = snapped.plateau;
                    drag.snappedValue = snapped.snappedValue;
                }
            } else {
                const next = curvePointAt(axes, position, sampleRate);
                index = next.sampleIndex;
                value = next.value;
            }
            if (index === undefined) return;
            if (drag.companions.length) {
                const { limits } = pointDrag;
                let delta = index - initial.sampleIndex!;
                if (!grab) delta = BigInt.asIntN(64, delta);
                if (limits.min !== undefined && delta < limits.min) delta = limits.min;
                if (limits.max !== undefined && delta > limits.max) delta = limits.max;
                index = initial.sampleIndex! + delta;
            }
            const sampleIndex = BigInt.asIntN(64, index);
            const authoredValue = Math.fround(value);
            const { base, neighbors } = pointDrag;
            const desired = { ...base, sampleIndex, seconds: Number(sampleIndex) / sampleRate, value: authoredValue,
                ...(time?.order !== undefined ? { order: time.order } : {}) };
            if (!grab && !drag.companions.length)
                time = { sampleIndex, order: desired.order, orderedIds: [], slot: pointInsertionIndex(neighbors, desired) };
            const previousSlot = pointDrag.time?.slot ?? pointDrag.initialSlot;
            const slotChanged = time !== undefined && time.slot !== previousSlot;
            if (!Number.isFinite(authoredValue) || (sampleIndex === drag.desired.sampleIndex && authoredValue === drag.desired.value
                && desired.order === drag.desired.order && !slotChanged)) {
                if (drag.snappedValue !== previousSnap) this.changed();
                return;
            }
            if (time && slotChanged && !pointDrag.adding) {
                const distance = BigInt.asIntN(64, sampleIndex - drag.desired.sampleIndex!);
                const forward = distance === 0n ? time.slot > previousSlot : distance > 0n;
                const crossed = forward ? previousSlot !== neighbors.length || time.slot !== 0
                    : previousSlot !== 0 || time.slot !== neighbors.length;
                // Going through the signed storage seam does not cross a point.
                // Keep tracking crossings in Shift mode for when Shift is released.
                if (crossed) pointDrag.gapAttachment = forward ? "left" : "right";
            }
            pointDrag.time = time;
            if (time) this.applyPlacement(drag, desired, time);
            else drag.desired = desired;
        }
        this.scheduleDrag(drag);
    }
    private applyPlacement(drag: Drag, point: AutomationPoint, time: PointTime): void {
        const edit = drag.pointDrag!;
        const { base, baseSegments, adding, neighbors } = edit;
        const placed = edit.placing && (!adding || point.sampleIndex !== base.sampleIndex || time.slot !== edit.initialSlot)
            ? placeAutomationPoint(neighbors, adding ? null : base, point, time.slot, edit.gapAttachment) : null;
        drag.desired = placed?.point ?? { ...point, interpolation: base.interpolation, tension: base.tension };
        edit.segments = placed?.segments ?? baseSegments;
        for (const id of edit.segments.keys()) edit.touched.add(id);
    }
    private scheduleDrag(drag: Drag): void {
        if (!drag.companions.length && drag.timer === undefined && !drag.pending) drag.timer = this.timers.set(() => {
            drag.timer = undefined;
            void this.flush(drag).catch(() => {});
        });
        this.changed();
    }
    private movedPoints(drag: Drag, anchor: AutomationPointPosition): AutomationPointPosition[] {
        if (!drag.companions.length) return [anchor];
        const timeDelta = anchor.sampleIndex! - drag.initial.sampleIndex!;
        const valueDelta = anchor.value - drag.initial.value;
        return [anchor, ...drag.companions.map((point) => {
            if (anchor === drag.initial) return point;
            const sampleIndex = BigInt.asIntN(64, point.sampleIndex! + timeDelta);
            const value = Math.fround(point.value + valueDelta);
            return { ...point, sampleIndex, seconds: Number(sampleIndex) / drag.sampleRate!, value };
        })];
    }
    private draftPoints(drag: Drag): AutomationPointPosition[] {
        const points = this.movedPoints(drag, drag.desired);
        const edit = drag.pointDrag;
        if (!edit || (!edit.touched.size && !edit.time?.orderedIds.length)) return points;
        const drafts = new Map(points.map((point) => [point.id, point]));
        const neighborDraft = (id: number) => {
            const { seconds, sampleIndex, value } = edit.byId.get(id)!;
            return { id, seconds, sampleIndex, value };
        };
        // A style preview leaves the neighbor's accepted order alone unless
        // it participates in the coincident order currently under the cursor.
        for (const id of edit.touched) drafts.set(id, { ...neighborDraft(id),
            ...(edit.segments.get(id) ?? segmentEdit(edit.byId.get(id)!)) });
        edit.time?.orderedIds.forEach((id, order) => drafts.set(id, { ...(drafts.get(id) ?? neighborDraft(id)), order }));
        return [...drafts.values()];
    }
    private flush(drag: Drag): Promise<void> {
        if (drag.pending) return drag.pending;
        drag.pending = (async () => {
            while (this.active === drag) {
                const { desired: point, sent, pointDrag: edit } = drag;
                const segments = edit ? segmentChanges(edit) : [];
                const orderedIds = edit?.time?.orderedIds;
                const styleChanged = !sameSegment(sent ?? undefined, point);
                const orderChanged = sent?.order !== point.order || (edit && !sameOrder(edit.sentOrder, orderedIds));
                if (sent?.sampleIndex === point.sampleIndex && sent?.value === point.value
                    && !styleChanged && !orderChanged && !segments.length) break;
                if (drag.kind !== "point") {
                    await drag.gesture.send(SetSegment(segmentEdit(point)));
                } else if (edit && (edit.adding || styleChanged || orderChanged || segments.length
                    || (orderedIds?.length && sent?.sampleIndex !== point.sampleIndex))) {
                    const desiredSegments = edit.segments;
                    await drag.gesture.send(PlacePoint({ insert: sent === null,
                        point: { id: point.id, seconds: 0, sample_index: point.sampleIndex!.toString(),
                            value: point.value, interpolation: point.interpolation, tension: point.tension, order: point.order ?? 0 },
                        segments, ordered_ids: [...(orderedIds ?? [])] }));
                    edit.sentSegments = desiredSegments;
                    edit.sentOrder = orderedIds ?? [];
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
    finish(deleteClick = false): Promise<void> {
        const drag = this.active;
        if (!drag) return Promise.resolve();
        if (drag.finishing) return drag.finishing;
        if (drag.timer !== undefined) this.timers.clear(drag.timer);
        drag.timer = undefined;
        const remove = deleteClick && drag.pointDrag?.deleteOnClick && !drag.pointDrag.moved;
        const pending = remove
            ? drag.gesture.send(RemovePoint({ id: drag.desired.id })).finally(() => drag.gesture.finish())
            : this.flush(drag).then(() => drag.gesture.finish());
        drag.finishing = pending.then(() => {
            const revision = drag.gesture.acceptedRevision?.();
            if (!remove && this.active === drag && revision !== undefined && this.readRevision < revision) {
                let entries = this.ending.get(drag.key);
                if (!entries) this.ending.set(drag.key, entries = new Map());
                for (const point of this.draftPoints(drag))
                    entries.set(point.id, { point, revision, axes: drag.axes });
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
        this.active = null; this.selection.clear(); this.curves.clear(); this.errors.clear(); this.ending.clear(); this.readRevision = 0; this.changed();
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

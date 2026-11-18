import { automationSampleIndex, compareAutomationPoints, type AutomationPoint } from "./automationCurveGeometry";
import type { SetSegmentPayload } from "./generated/automationEvents";
import { detentHalfWidth, ScrubSensitivity } from "./scrubSensitivity";

export function pointInsertionIndex(points: readonly AutomationPoint[], point: AutomationPoint): number {
    let first = 0, end = points.length;
    while (first < end) {
        const middle = first + Math.floor((end - first) / 2);
        if (compareAutomationPoints(points[middle], point) <= 0) first = middle + 1;
        else end = middle;
    }
    return first;
}

const sampleDistance = (from: bigint, to: bigint) => BigInt.asUintN(64, to - from);
const pointBefore = (points: readonly AutomationPoint[], index: number) => points[(index + points.length - 1) % points.length];
const bezier = { interpolation: "bezier", tension: 1 } as const;
const empty = { interpolation: "empty", tension: 1 } as const;
const segmentOf = ({ id, interpolation, tension }: AutomationPoint): SetSegmentPayload => ({ id, interpolation, tension });
export type GapAttachment = "left" | "right" | "nearest" | null;

export function initialGapAttachment(points: readonly AutomationPoint[], point: AutomationPoint, slot: number): GapAttachment {
    if (point.interpolation !== "empty") return "right";
    if (points.length && pointBefore(points, slot).interpolation !== "empty") return "left";
    return null;
}

export function placeAutomationPoint(points: readonly AutomationPoint[], original: AutomationPoint | null,
    point: AutomationPoint, slot: number, attachment: GapAttachment): { point: AutomationPoint; segments: Map<number, SetSegmentPayload> } {
    const segments = new Map<number, SetSegmentPayload>();
    if (!points.length) return { point, segments };
    const sourceSlot = original ? pointInsertionIndex(points, original) : -1;
    const before = pointBefore(points, slot), after = points[slot % points.length];
    const sourceBefore = original ? pointBefore(points, sourceSlot) : undefined;
    const sourceAfter = original ? points[sourceSlot % points.length] : undefined;
    const setSegment = (neighbor: AutomationPoint, style: Pick<AutomationPoint, "interpolation" | "tension">) => {
        if (neighbor.interpolation === style.interpolation && neighbor.tension === style.tension) segments.delete(neighbor.id);
        else segments.set(neighbor.id, { id: neighbor.id, interpolation: style.interpolation, tension: style.tension });
    };
    // Removing the end of a covered segment must leave a gap where it used
    // to end. A later point at the same sample already owns the outgoing span.
    if (original?.interpolation === "empty" && sourceBefore && sourceBefore.interpolation !== "empty"
        && (sourceSlot === points.length || sourceAfter?.sampleIndex !== original.sampleIndex)) {
        setSegment(sourceBefore, { interpolation: "empty", tension: sourceBefore.tension });
    }
    const incoming = segments.get(before.id) ?? segmentOf(before);
    if (incoming.interpolation !== "empty") {
        const style = original && original.interpolation !== "empty" && sourceBefore === before && sourceAfter === after
            ? original : incoming;
        return { point: { ...point, interpolation: style.interpolation, tension: style.tension }, segments };
    }
    if (attachment === null) return { point, segments };

    const left = sampleDistance(before.sampleIndex!, point.sampleIndex!);
    const right = sampleDistance(point.sampleIndex!, after.sampleIndex!);
    // A zero distance at the cyclic seam can mean a whole turn. If both
    // neighbors are coincident, the chosen slot identifies that side.
    const nearestIsLeft = left < right || (left === 0n && right === 0n && slot === points.length);
    const attachLeft = attachment === "left" || (attachment === "nearest" && nearestIsLeft);
    if (attachLeft) {
        const start = pointInsertionIndex(points, { ...before, order: -Infinity });
        const departing = before === sourceBefore && segments.has(before.id);
        const edge = pointBefore(points, start);
        const style = departing ? before : segments.get(edge.id) ?? edge;
        setSegment(before, style.interpolation === "empty" ? bezier : style);
        const styleOfPoint = original?.interpolation === "empty" ? original : empty;
        return { point: { ...point, interpolation: "empty", tension: styleOfPoint.tension }, segments };
    }
    const end = pointInsertionIndex(points, { ...after, order: Infinity });
    const edge = points[end - 1];
    const style = edge === sourceBefore && segments.has(edge.id) ? edge
        : original && original.interpolation !== "empty" && sourceBefore?.interpolation === "empty" && after === sourceAfter ? original
            : segments.get(edge.id) ?? edge;
    const covered = style.interpolation === "empty" ? bezier : style;
    return { point: { ...point, interpolation: covered.interpolation, tension: covered.tension }, segments };
}

type SampleGroup = { start: number; points: AutomationPoint[] };
export type PointTime = { sampleIndex: bigint; order?: number; orderedIds: readonly number[]; slot: number };

export function automationTimeDrag(neighbors: readonly AutomationPoint[], anchor: AutomationPoint,
    secondsPerPixel: number, sampleRate: number): (pixels: number) => PointTime {
    const groups = new Map<bigint, SampleGroup>();
    for (const [index, point] of neighbors.entries()) {
        const sample = point.sampleIndex!;
        const group = groups.get(sample);
        if (group) group.points.push(point);
        else groups.set(sample, { start: index, points: [point] });
    }
    const sample = anchor.sampleIndex!;
    const samplesPerPixel = secondsPerPixel * sampleRate;
    const targets = new Map<number, bigint>();
    for (const index of groups.keys()) {
        targets.set(Number(sampleDistance(sample, index)) / samplesPerPixel, index);
        targets.set(-Number(sampleDistance(index, sample)) / samplesPerPixel, index);
    }
    const offsets = [...targets.keys()].sort((a, b) => a - b);
    const scrub = new ScrubSensitivity(1, 0, 0);
    const anchorGroup = groups.get(sample);
    const anchorSlot = pointInsertionIndex(neighbors, anchor);
    const anchorPlateau = anchorGroup && anchorSlot === anchorGroup.start ? -detentHalfWidth / 2 : detentHalfWidth / 2;
    return (pixels) => {
        const detent = scrub.valueAtDetents(pixels, offsets, anchorPlateau);
        const snapped = detent.snappedValue === undefined ? undefined : targets.get(detent.snappedValue);
        const delta = automationSampleIndex(detent.value * secondsPerPixel, sampleRate);
        const sampleIndex = snapped ?? BigInt.asIntN(64, sample + (delta ?? 0n));
        const group = groups.get(sampleIndex);
        if (!group) return { sampleIndex, order: anchor.order, orderedIds: [], slot: pointInsertionIndex(neighbors, { ...anchor, sampleIndex }) };
        // The first half places the grabbed point before the coincident group.
        // The second half places it after, using the very same sample index.
        const beforeGroup = snapped !== undefined ? detent.plateau < 0 : detent.value < Number(delta ?? 0n) / samplesPerPixel;
        const order = pixels === 0 ? anchorSlot - group.start : beforeGroup ? 0 : group.points.length;
        const orderedIds = group.points.map(({ id }) => id);
        orderedIds.splice(order, 0, anchor.id);
        return { sampleIndex, order, orderedIds, slot: group.start + order };
    };
}

import { valueScaleFraction } from "./knobPresentation";
import { signedTimelineSpans, timelineSampleCount, timelineSampleExtent, type SamplePosition, type TimelineSamples } from "./timelineSamples";

export type CurveInterpolation = "empty" | "hold" | "bezier";
export type AutomationPointPosition = { id: number; seconds: number; value: number; sampleIndex?: bigint;
    tension?: number; interpolation?: CurveInterpolation; order?: number };
export type AutomationPoint = AutomationPointPosition & { interpolation: CurveInterpolation; tension: number };
export type AutomationCurve = { points: AutomationPoint[]; cycleSeconds: number; sampleRate?: number };
export type AutomationValueBounds = { min?: number; max?: number };
export type AutomationValueDisplay = "curved" | "cropped";
export type AutomationValueZoom = { factor: number; center: number };
export const minimumAutomationValueZoom = 2 ** -20;
export const maximumAutomationValueZoom = 2 ** 20;
export type AutomationValueAxis = { pan?: number; display?: AutomationValueDisplay; zoom?: AutomationValueZoom };
export function clampAutomationValue(value: number, { min, max }: AutomationValueBounds): number {
    if (min !== undefined && max !== undefined && min > max) return (min + max) / 2;
    return Math.min(max ?? Infinity, Math.max(min ?? -Infinity, value));
}

export type AutomationValueRuler = "auto" | "whole" | "half" | "off";
export type AutomationCurveView = AutomationValueAxis & { ruler?: AutomationValueRuler };

export function readAutomationCurveView(value: unknown): AutomationCurveView {
    if (!value || typeof value !== "object") return {};
    const { ruler, display, pan, zoom } = value as AutomationCurveView;
    const view: AutomationCurveView = {};
    if (ruler === "whole" || ruler === "half" || ruler === "off") view.ruler = ruler;
    if (display === "cropped") view.display = display;
    if (Number.isFinite(pan) && pan !== 0) view.pan = pan;
    if (zoom && Number.isFinite(zoom.factor) && zoom.factor >= minimumAutomationValueZoom
        && zoom.factor <= maximumAutomationValueZoom && zoom.factor !== 1 && Number.isFinite(zoom.center)
        && (view.display === "cropped" || zoom.factor < 1 ? zoom.center === .5
            : zoom.center >= .5 / zoom.factor && zoom.center <= 1 - .5 / zoom.factor)) {
        view.zoom = { factor: zoom.factor, center: zoom.center };
    }
    return view;
}

export type CurvePath = { begin: { seconds: number; value: number }; end: { seconds: number; value: number };
    first: number; second: number };

export function curveTangent(previous: number, next: number, previousLength: number, nextLength: number): number {
    if (!previous || !next || Math.sign(previous) !== Math.sign(next)) return 0;
    const first = 2 * nextLength + previousLength, second = nextLength + 2 * previousLength;
    return (first + second) / (first / previous + second / next);
}

export function automationCycleSeconds(sampleRate: number | null | undefined): number {
    return sampleRate && sampleRate > 0 ? 2 ** 64 / sampleRate : Infinity;
}

function samplePosition(seconds: number, sampleRate: number, offset = 0) {
    if (!Number.isFinite(seconds) || !Number.isSafeInteger(sampleRate) || sampleRate <= 0) return;
    const whole = Math.trunc(seconds), wholeOffset = Math.trunc(offset);
    const samples = (seconds - whole + (offset - wholeOffset)) * sampleRate;
    if (!Number.isFinite(samples)) return;
    const floor = Math.floor(samples);
    return { index: (BigInt(whole) + BigInt(wholeOffset)) * BigInt(sampleRate) + BigInt(floor), fraction: samples - floor };
}

export function automationSampleIndex(seconds: number, sampleRate: number, offset = 0): bigint | undefined {
    const position = samplePosition(seconds, sampleRate, offset);
    if (!position) return;
    const { index, fraction } = position;
    return fraction > .5 || (fraction === .5 && index >= 0n) ? index + 1n : index;
}

export function automationSampleOffset(index: bigint, origin: number, sampleRate: number): number {
    const position = samplePosition(origin, sampleRate);
    return position ? (Number(index - position.index) - position.fraction) / sampleRate : NaN;
}

export function compareAutomationTime(a: AutomationPointPosition, b: AutomationPointPosition): number {
    if (a.sampleIndex !== undefined && b.sampleIndex !== undefined)
        return a.sampleIndex < b.sampleIndex ? -1 : a.sampleIndex > b.sampleIndex ? 1 : 0;
    return a.seconds - b.seconds;
}

export function compareAutomationPoints(a: AutomationPointPosition, b: AutomationPointPosition): number {
    return compareAutomationTime(a, b) || (a.order ?? 0) - (b.order ?? 0) || a.id - b.id;
}

function croppedValueSpan({ pan = 0, zoom }: AutomationValueAxis): number {
    const precision = Math.abs(pan) * 2 ** -23;
    return 2 * Math.max(precision, 1 / (zoom?.factor ?? 1));
}

export function panAutomationValueAxis(axis: AutomationValueAxis, dy: number, height: number): number {
    const pan = axis.pan ?? 0;
    if (!dy || !(height > 0)) return pan;
    if (axis.display === "cropped") {
        const next = pan + croppedValueSpan(axis) * dy / height;
        return Number.isFinite(Math.fround(next)) ? next : pan;
    }
    const scale = Math.min(1, axis.zoom?.factor ?? 1);
    const next = pan + Math.PI * dy / (height * scale);
    return Number.isFinite(next) ? next : pan;
}

function curvedZoomCenter(center: number, factor: number): number {
    const half = .5 / factor;
    return Math.min(1 - half, Math.max(half, center));
}

export function panCurvedAutomationValueZoom(zoom: AutomationValueZoom, dy: number, height: number): AutomationValueZoom {
    return { factor: zoom.factor, center: curvedZoomCenter(zoom.center + dy / (height * zoom.factor), zoom.factor) };
}

export function zoomAutomationValueAxis(axis: AutomationValueAxis, multiplier: number, anchor: number):
    { pan: number; zoom: AutomationValueZoom } {
    const pan = axis.pan ?? 0;
    const zoom = axis.zoom ?? { factor: 1, center: .5 };
    const factor = Math.max(minimumAutomationValueZoom, Math.min(maximumAutomationValueZoom, zoom.factor * multiplier));
    const fraction = Number.isFinite(anchor) ? anchor : .5;
    if (factor === zoom.factor) return { pan, zoom };
    if (axis.display === "cropped") {
        const value = automationValueAtFraction(fraction, axis);
        const change = { factor, center: .5 };
        const span = croppedValueSpan({ ...axis, zoom: change });
        return { pan: value - (fraction - .5) * span, zoom: change };
    }
    // Below 1 the scale widens in value space. Above 1 the view crops the
    // curved scale in fraction space. Apply both changes when crossing 1.
    const offset = fraction - .5;
    const wideChange = 1 / Math.min(1, zoom.factor) - 1 / Math.min(1, factor);
    let nextPan = pan;
    if (wideChange) {
        // Continue the inverse curve along its tangent outside the middle
        // half, so the anchor remains finite and keeps moving through wraps.
        const inner = Math.min(.25, Math.max(-.25, offset));
        const anchorValue = Math.tan(Math.PI * inner) + (offset - inner) * (2 * Math.PI);
        const candidate = pan + anchorValue * wideChange;
        if (Number.isFinite(candidate)) nextPan = candidate;
    }
    const cropFactor = Math.max(1, factor);
    const cropChange = 1 / Math.max(1, zoom.factor) - 1 / cropFactor;
    const center = curvedZoomCenter(zoom.center + offset * cropChange, cropFactor);
    return { pan: nextPan, zoom: { factor, center } };
}

export function automationValueFraction(value: number, axis: AutomationValueAxis): number {
    const { pan = 0, zoom } = axis;
    if (axis.display === "cropped") return .5 + (value - pan) / croppedValueSpan(axis);
    const scale = Math.min(1, zoom?.factor ?? 1);
    const fraction = valueScaleFraction((value - pan) * scale, {});
    return zoom && zoom.factor > 1 ? .5 + (fraction - zoom.center) * zoom.factor : fraction;
}

export function automationValueAtFraction(fraction: number, axis: AutomationValueAxis): number {
    const { pan = 0, zoom } = axis;
    if (axis.display === "cropped") return pan + (fraction - .5) * croppedValueSpan(axis);
    if (zoom && zoom.factor > 1) fraction = zoom.center + (fraction - .5) / zoom.factor;
    const scale = Math.min(1, zoom?.factor ?? 1);
    return pan + Math.tan((fraction - .5) * Math.PI) / scale;
}

export function automationUnitsPerPixel(axis: AutomationValueAxis, height: number): number {
    if (axis.display === "cropped") return croppedValueSpan(axis) / height;
    if (axis.zoom && axis.zoom.factor !== 1)
        return automationValueAtFraction(.5 + .5 / height, axis) - automationValueAtFraction(.5 - .5 / height, axis);
    return Math.PI / height;
}

export function automationValueGuides(axis: AutomationValueAxis, height: number,
    ruler: AutomationValueRuler = "auto"): Array<{ value: number; y: number }> {
    if (ruler === "off" || !(height > 0) || !Number.isFinite(height)) return [];
    const guides: Array<{ value: number; y: number }> = [];
    function add(candidate: number, spacing: number) {
        const rounded = Math.fround(candidate);
        if (!Number.isFinite(rounded)) return;
        const value = rounded === 0 ? 0 : rounded;
        const y = (1 - automationValueFraction(value, axis)) * height;
        if (!(y >= 0 && y <= height) || guides.some((guide) => Math.abs(guide.y - y) < spacing)) return;
        guides.push({ value, y });
    }
    // Keep central references and whole values first. Only add a subdivision
    // where it fits between the guides already visible on the value scale.
    for (const value of [0, -1, 1]) add(value, 10);
    const spacing = 18;
    const count = Math.min(100, Math.floor(height / spacing));
    const values = [automationValueAtFraction(.5, axis)];
    for (let i = 1; i <= count; ++i) {
        const offset = i / (2 * count + 1);
        values.push(automationValueAtFraction(.5 - offset, axis), automationValueAtFraction(.5 + offset, axis));
    }
    // Beyond one pixel of finest spacing, halving again has no visible benefit.
    // 2^-149 is the smallest positive float32 Sample.
    const smallestSample = 2 ** -149;
    const smallestStep = ruler === "auto" ? automationUnitsPerPixel(axis, height)
        : ruler === "half" ? .5 : 1;
    for (let step = 1; step >= smallestSample; step /= 2) {
        for (const value of values) add(Math.round(value / step) * step, spacing);
        if (step <= smallestStep) break;
    }
    return guides.sort((a, b) => a.value - b.value);
}

export function rankAutomationValueGuides(guides: ReturnType<typeof automationValueGuides>) {
    // The displayed Sample values are exact multiples of a power of two.
    // Use their finest step so whole-number grids and zoomed-in grids both have tiers.
    let step = Infinity;
    for (const { value } of guides) {
        if (value === 0) continue;
        step = Math.min(step, 2 ** Math.floor(Math.log2(Math.abs(value))));
        while (!Number.isInteger(value / step)) step /= 2;
    }
    return guides.map((guide) => {
        const index = guide.value / step;
        const depth = guide.value === 0 ? 0 : Math.abs(guide.value) === 1 || index % 4 === 0 ? 1 : index % 2 === 0 ? 2 : 3;
        return { ...guide, depth };
    });
}

// Only the viewport and its neighboring point groups are visited. Extra end
// groups supply tangents across the cyclic seam; drafts replace point identity.
export function automationWindow(curve: AutomationCurve, begin: number, end: number,
    drafts: readonly AutomationPointPosition[] = [], origin?: number | SamplePosition) {
    // The clip uses a local time coordinate so it can distinguish samples even
    // when adding one sample to an absolute seconds value would round away.
    const time = (point: AutomationPoint, cycle = 0) => {
        if (typeof origin === "object") return (Number(point.sampleIndex! - origin.index + BigInt(cycle) * timelineSampleCount)
            - origin.fraction) / curve.sampleRate!;
        const seconds = origin === undefined ? point.seconds : point.sampleIndex !== undefined && curve.sampleRate
            ? automationSampleOffset(point.sampleIndex, origin, curve.sampleRate) : point.seconds - origin;
        return cycle ? seconds + cycle * curve.cycleSeconds : seconds;
    };
    const boundary = (seconds: number, after: boolean) => {
        let first = 0, last = curve.points.length;
        while (first < last) {
            const middle = first + Math.floor((last - first) / 2);
            const position = time(curve.points[middle]);
            if (position < seconds || (after && position === seconds)) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const neighbors = drafts.length + 2;
    const collect = (start: number, finish: number) => {
        for (let i = 0; i < neighbors && start > 0; ++i) start = boundary(time(curve.points[start - 1]), false);
        for (let i = 0; i < neighbors && finish < curve.points.length; ++i) finish = boundary(time(curve.points[finish]), true);
        return curve.points.slice(start, finish);
    };
    const selected = new Map(collect(boundary(begin, false), boundary(end, true)).map((point) => [point.id, point]));
    for (const point of [...collect(0, 0), ...collect(curve.points.length, curve.points.length)]) selected.set(point.id, point);
    // Draft identities are indexed once per accepted curve, rather than scanned
    // through the complete point list on every pointer movement.
    const byId = automationPointOrder(curve);
    for (const draft of drafts) {
        const original = curve.points[byId.get(draft.id) ?? -1];
        if (original) selected.set(draft.id, { ...original, ...draft });
        else if (draft.interpolation !== undefined && draft.tension !== undefined)
            selected.set(draft.id, { ...draft, interpolation: draft.interpolation, tension: draft.tension });
    }
    const points = [...selected.values()].sort(compareAutomationPoints);
    const groups: Array<{ first: AutomationPoint; last: AutomationPoint }> = [];
    for (const point of points) {
        const last = groups.at(-1);
        if (last && compareAutomationTime(last.first, point) === 0) last.last = point;
        else groups.push({ first: point, last: point });
    }
    const forward = (from: number, to: number) => to >= from ? to - from : to - from + curve.cycleSeconds;
    const pointDistance = (from: AutomationPoint, to: AutomationPoint) => curve.sampleRate
        && from.sampleIndex !== undefined && to.sampleIndex !== undefined
        ? Number(BigInt.asUintN(64, to.sampleIndex - from.sampleIndex)) / curve.sampleRate
        : forward(time(from), time(to));
    const paths: CurvePath[] = [];
    const sampleSegments: Array<{ start: number; finish: number; sample: (seconds: number) => number }> = [];
    for (let i = 0; i < groups.length; ++i) {
        const group = groups[i], next = groups[(i + 1) % groups.length];
        const before = group.last, after = next.first;
        if (before.interpolation === "empty") continue;
        const beforeTime = time(before), afterTime = time(after);
        const single = groups.length === 1;
        const length = single ? curve.cycleSeconds : pointDistance(before, after);
        let startDelta = 0, endDelta = 0;
        const difference = single ? 0 : after.value - before.value;
        if (!single && before.interpolation === "bezier" && before.tension > 0 && Number.isFinite(length)) {
            const previous = groups[(i + groups.length - 1) % groups.length].last;
            const following = groups[(i + 2) % groups.length].first;
            const slope = difference / length;
            const previousLength = pointDistance(previous, before);
            const nextLength = pointDistance(after, following);
            if (previous.interpolation === "bezier" && group.first.value === before.value && Number.isFinite(previousLength))
                startDelta = curveTangent((before.value - previous.value) / previousLength, slope, previousLength, length) * length;
            if (next.last.interpolation === "bezier" && after.value === next.last.value && Number.isFinite(nextLength))
                endDelta = curveTangent(slope, (following.value - after.value) / nextLength, length, nextLength) * length;
        }
        const ratios = (seconds: number, start: number, finish: number) => ({
            t: (seconds - start) / length,
            u: (finish - seconds) / length,
        });
        const valueAt = (seconds: number, start: number, finish: number) => {
            if (single || before.interpolation === "hold") return before.value;
            if (!Number.isFinite(length)) return after.value;
            const { t, u } = ratios(seconds, start, finish);
            const linear = t <= u ? before.value + difference * t : after.value - difference * u;
            const value = linear + before.tension * t * u * ((startDelta - difference) * u + (difference - endDelta) * t);
            return Math.max(Math.min(before.value, after.value), Math.min(Math.max(before.value, after.value), value));
        };
        const derivative = (seconds: number, start: number, finish: number) => {
            if (single || before.interpolation === "hold" || !Number.isFinite(length)) return 0;
            const { t, u } = ratios(seconds, start, finish);
            const automatic = startDelta * (u * u - 2 * t * u) + endDelta * (t * t - 2 * t * u) + 6 * difference * t * u;
            return (difference + before.tension * (automatic - difference)) / length;
        };
        // A single nonempty point covers the circle; viewport clipping only
        // limits the path, not the coverage used to sample its right edge.
        const intervals = single ? [[-Infinity, Infinity]] : beforeTime < afterTime
            ? [[beforeTime, afterTime]]
            : compareAutomationTime(before, after) < 0 ? []
                : [[beforeTime, time(after, 1)], [time(before, -1), afterTime]];
        for (const [start, finish] of intervals) {
            const a = Math.max(begin, start), b = Math.min(end, finish);
            if (!(a < b)) continue;
            const sample = (seconds: number) => valueAt(seconds, start, finish);
            const first = sample(a), last = sample(b);
            // At the next point, an incoming endpoint precedes any outgoing
            // jump. Use its left-hand limit for this segment's terminal value.
            const terminal = !single && b === finish && before.interpolation === "bezier" ? after.value : last;
            paths.push({ begin: { seconds: a, value: first }, end: { seconds: b, value: terminal },
                first: first + derivative(a, start, finish) * (b - a) / 3,
                second: terminal - derivative(b, start, finish) * (b - a) / 3 });
            sampleSegments.push({ start, finish, sample });
        }
    }
    paths.sort((a, b) => a.begin.seconds - b.begin.seconds);
    const visible = points.filter((point) => time(point) >= begin && time(point) <= end);
    const valueAt = (seconds: number) => sampleSegments.find((segment) => seconds >= segment.start && seconds < segment.finish)?.sample(seconds);
    const vertices: Array<{ seconds: number; value: number }> = [];
    const first = valueAt(begin), last = valueAt(end);
    if (first !== undefined) vertices.push({ seconds: begin, value: first });
    for (const point of visible) vertices.push({ seconds: time(point), value: point.value });
    if (last !== undefined) vertices.push({ seconds: end, value: last });
    return { points: visible, paths, vertices };
}

export function automationTimelineWindow(curve: AutomationCurve, view: TimelineSamples,
    drafts: readonly AutomationPointPosition[] = []) {
    const rate = curve.sampleRate!;
    const points = new Map<number, AutomationPoint>();
    const paths: CurvePath[] = [];
    const parts = signedTimelineSpans(view.origin.index, timelineSampleExtent(view));
    for (const part of parts) {
        const first = part.offset === 0n;
        const offset = first ? 0 : (Number(part.offset) - view.origin.fraction) / rate;
        const origin = { index: part.begin, fraction: first ? view.origin.fraction : 0 };
        const end = Math.min(view.span / rate - offset, (Number(part.length) - origin.fraction) / rate);
        if (!(end > 0)) continue;
        const visible = automationWindow(curve, 0, end, drafts, origin);
        for (const point of visible.points) points.set(point.id, point);
        for (const path of visible.paths) paths.push({ ...path,
            begin: { ...path.begin, seconds: path.begin.seconds + offset },
            end: { ...path.end, seconds: path.end.seconds + offset } });
    }
    return { points: [...points.values()], paths };
}

const indexes = new WeakMap<AutomationCurve, Map<number, number>>();
function automationPointOrder(curve: AutomationCurve) {
    let index = indexes.get(curve);
    if (!index) {
        index = new Map(curve.points.map((point, position) => [point.id, position]));
        indexes.set(curve, index);
    }
    return index;
}

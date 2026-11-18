import { valueScaleFraction } from "./knobPresentation";

export type CurveInterpolation = "empty" | "hold" | "bezier";
export type AutomationPointPosition = { id: number; seconds: number; value: number; sampleIndex?: bigint;
    tension?: number; interpolation?: CurveInterpolation; order?: number };
export type AutomationPoint = AutomationPointPosition & { interpolation: CurveInterpolation; tension: number };
export type AutomationCurve = { points: AutomationPoint[]; cycleSeconds: number; sampleRate?: number; bounds?: AutomationBoundSettings };
export type AutomationValueBounds = { min?: number; max?: number };
export type AutomationValueDisplay = "curved" | "cropped";
export type AutomationValueZoom = { factor: number; center: number };
export const minimumAutomationValueZoom = 2 ** -20;
export const maximumAutomationValueZoom = 2 ** 20;
export type AutomationValueAxis = AutomationValueBounds & { pan?: number; display?: AutomationValueDisplay; zoom?: AutomationValueZoom };
export type AutomationBoundSide = "min" | "max";
export type AutomationBound = { value: number; enabled: boolean };
export type AutomationBoundSettings = Record<AutomationBoundSide, AutomationBound>;
export const defaultAutomationBounds: AutomationBoundSettings = {
    min: { value: 0, enabled: false },
    max: { value: 1, enabled: false },
};

export function enabledAutomationBounds(settings?: AutomationBoundSettings): AutomationValueBounds {
    return { min: settings?.min.enabled ? settings.min.value : undefined,
        max: settings?.max.enabled ? settings.max.value : undefined };
}

export function automationBoundLimits(settings: AutomationBoundSettings, side: AutomationBoundSide): AutomationValueBounds {
    return side === "min" ? { max: settings.max.enabled ? settings.max.value : undefined }
        : { min: settings.min.enabled ? settings.min.value : undefined };
}

export function clampAutomationValue(value: number, { min, max }: AutomationValueBounds): number {
    return Math.min(max ?? Infinity, Math.max(min ?? -Infinity, value));
}

export function automationDisplayBounds(bounds: AutomationValueBounds): AutomationValueBounds {
    const { min, max } = bounds;
    if (min === undefined || min !== max) return bounds;
    // An equal pair produces a constant signal. Keep an editable value axis
    // centred on it so raw values remain adjustable.
    const radius = Math.max(1, Math.abs(min) * .1);
    return { min: min - radius, max: min + radius };
}
export type AutomationValueRuler = "auto" | "whole" | "half" | "off";
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

const clampOneSidedPan = (pan: number) => Math.max(-16, Math.min(16, pan));
function oneSidedScale(bound: number, pan: number): number {
    return Math.max(1, Math.abs(bound)) * Math.exp(clampOneSidedPan(pan));
}

function croppedValueWindow(axis: AutomationValueAxis) {
    const { min, max, pan = 0, zoom } = axis;
    const range = min !== undefined && max !== undefined ? max - min : Infinity;
    const precision = Math.abs(clampAutomationValue(pan, axis)) * 2 ** -23;
    const half = Math.max(precision, Math.min(1, range > 0 ? range / 2 : 1) / (zoom?.factor ?? 1));
    if (range <= 2 * half)
        return { center: min! + range / 2, span: range || 2 * half };
    const center = clampAutomationValue(pan, {
        min: min === undefined ? undefined : min + half,
        max: max === undefined ? undefined : max - half,
    });
    return { center, span: 2 * half };
}

export function canPanAutomationValueAxis(axis: AutomationValueAxis): boolean {
    if (axis.display !== "cropped" && (axis.zoom?.factor ?? 1) > 1) return true;
    if (axis.min === undefined || axis.max === undefined) return true;
    return axis.display === "cropped" && axis.max - axis.min > croppedValueWindow(axis).span;
}

export function panAutomationValueAxis(axis: AutomationValueAxis, dy: number, height: number): number {
    const pan = axis.pan ?? 0;
    if (!dy || !(height > 0) || !canPanAutomationValueAxis(axis)) return pan;
    if (axis.display === "cropped") {
        const { center, span } = croppedValueWindow(axis);
        const next = center + span * dy / height;
        return Number.isFinite(Math.fround(next)) ? croppedValueWindow({ ...axis, pan: next }).center : center;
    }
    const unbounded = axis.min === undefined && axis.max === undefined;
    const scale = unbounded ? Math.min(1, axis.zoom?.factor ?? 1) : 1;
    const next = pan + Math.PI * dy / (height * scale);
    if (!Number.isFinite(next)) return pan;
    return unbounded ? next : clampOneSidedPan(next);
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
    const range = axis.min !== undefined && axis.max !== undefined ? axis.max - axis.min : Infinity;
    const minimum = axis.display === "cropped"
        ? Math.max(minimumAutomationValueZoom, range > 0 ? Math.min(1, 2 / range) : 0)
        : Number.isFinite(range) ? 1 : minimumAutomationValueZoom;
    const factor = Math.max(minimum, Math.min(maximumAutomationValueZoom, zoom.factor * multiplier));
    const fraction = Number.isFinite(anchor) ? anchor : .5;
    if (factor === zoom.factor) return { pan, zoom };
    if (axis.display === "cropped") {
        const value = automationValueAtFraction(fraction, axis);
        const change = { factor, center: .5 };
        const { span } = croppedValueWindow({ ...axis, zoom: change });
        const center = croppedValueWindow({ ...axis, zoom: change, pan: value - (fraction - .5) * span }).center;
        return { pan: center, zoom: change };
    }
    // Below 1 the scale widens in value space. Above 1 the view crops the
    // curved scale in fraction space. Apply both changes when crossing 1.
    const offset = fraction - .5;
    const wideChange = 1 / Math.min(1, zoom.factor) - 1 / Math.min(1, factor);
    let nextPan = pan;
    if (wideChange && axis.min === undefined && axis.max === undefined) {
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
    const { min, max, pan = 0, zoom } = axis;
    if (axis.display === "cropped") {
        const { center, span } = croppedValueWindow(axis);
        return .5 + (value - center) / span;
    }
    const scale = Math.min(1, zoom?.factor ?? 1);
    let fraction: number;
    if (min !== undefined && max !== undefined) fraction = valueScaleFraction(value, { min, max });
    else if (min !== undefined) fraction = 2 * Math.atan((value - min) * scale / oneSidedScale(min, pan)) / Math.PI;
    else if (max !== undefined) fraction = 1 - 2 * Math.atan((max - value) * scale / oneSidedScale(max, -pan)) / Math.PI;
    else fraction = valueScaleFraction((value - pan) * scale, {});
    return zoom && zoom.factor > 1 ? .5 + (fraction - zoom.center) * zoom.factor : fraction;
}

export function automationValueAtFraction(fraction: number, axis: AutomationValueAxis): number {
    const { min, max, pan = 0, zoom } = axis;
    if (axis.display === "cropped") {
        const { center, span } = croppedValueWindow(axis);
        return center + (fraction - .5) * span;
    }
    if (zoom && zoom.factor > 1) fraction = zoom.center + (fraction - .5) / zoom.factor;
    const scale = Math.min(1, zoom?.factor ?? 1);
    if (min !== undefined && max !== undefined) return min + fraction * (max - min);
    if (min !== undefined) return min + Math.tan(fraction * Math.PI / 2) * oneSidedScale(min, pan) / scale;
    if (max !== undefined) return max + Math.tan((fraction - 1) * Math.PI / 2) * oneSidedScale(max, -pan) / scale;
    return pan + Math.tan((fraction - .5) * Math.PI) / scale;
}

export function automationUnitsPerPixel(axis: AutomationValueAxis, height: number): number {
    const { min, max, pan = 0 } = axis;
    if (axis.display === "cropped") return croppedValueWindow(axis).span / height;
    if (axis.zoom && axis.zoom.factor !== 1)
        return automationValueAtFraction(.5 + .5 / height, axis) - automationValueAtFraction(.5 - .5 / height, axis);
    if (min !== undefined && max !== undefined) return (max - min) / height;
    const scale = min !== undefined ? oneSidedScale(min, pan) / 2 : max !== undefined ? oneSidedScale(max, -pan) / 2 : 1;
    return Math.PI * scale / height;
}

export function automationValueGuides(bounds: AutomationValueAxis, height: number,
    ruler: AutomationValueRuler = "auto"): Array<{ value: number; y: number }> {
    if (ruler === "off" || !(height > 0) || !Number.isFinite(height)) return [];
    const guides: Array<{ value: number; y: number }> = [];
    function add(candidate: number, spacing: number) {
        const rounded = Math.fround(candidate);
        if (!Number.isFinite(rounded)) return;
        const value = rounded === 0 ? 0 : rounded;
        const y = (1 - automationValueFraction(value, bounds)) * height;
        if (!(y >= 0 && y <= height) || guides.some((guide) => Math.abs(guide.y - y) < spacing)) return;
        guides.push({ value, y });
    }
    // Keep central references and whole values first. Only add a subdivision
    // where it fits between the guides already visible on the value scale.
    for (const value of [0, -1, 1]) add(value, 10);
    const spacing = 18;
    const count = Math.min(100, Math.floor(height / spacing));
    const values = [automationValueAtFraction(.5, bounds)];
    for (let i = 1; i <= count; ++i) {
        const offset = i / (2 * count + 1);
        values.push(automationValueAtFraction(.5 - offset, bounds), automationValueAtFraction(.5 + offset, bounds));
    }
    // Beyond one pixel of finest spacing, halving again has no visible benefit.
    // 2^-149 is the smallest positive float32 Sample.
    const smallestSample = 2 ** -149;
    const smallestStep = ruler === "auto" ? automationUnitsPerPixel(bounds, height)
        : ruler === "half" ? .5 : 1;
    for (let step = 1; step >= smallestSample; step /= 2) {
        for (const value of values) add(Math.round(value / step) * step, spacing);
        if (step <= smallestStep) break;
    }
    return guides.sort((a, b) => a.value - b.value);
}

// Only the viewport and its neighboring point groups are visited. Extra end
// groups supply tangents across the cyclic seam; drafts replace point identity.
export function automationWindow(curve: AutomationCurve, begin: number, end: number,
    drafts: readonly AutomationPointPosition[] = [], origin?: number,
    bounds = enabledAutomationBounds(curve.bounds)) {
    // The clip uses a local time coordinate so it can distinguish samples even
    // when adding one sample to an absolute seconds value would round away.
    const time = (point: AutomationPoint) => origin === undefined ? point.seconds
        : point.sampleIndex !== undefined && curve.sampleRate ? automationSampleOffset(point.sampleIndex, origin, curve.sampleRate)
            : point.seconds - origin;
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
    for (const authored of points) {
        const value = clampAutomationValue(authored.value, bounds);
        const point = value === authored.value ? authored : { ...authored, value };
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
        const ratios = (seconds: number) => {
            const elapsed = forward(beforeTime, seconds), remaining = forward(seconds, afterTime);
            return { elapsed, remaining, t: Number.isFinite(elapsed) ? elapsed / length : 1,
                u: Number.isFinite(remaining) ? remaining / length : 1 };
        };
        const sample = (seconds: number) => {
            if (single || before.interpolation === "hold") return before.value;
            const { elapsed, remaining, t, u } = ratios(seconds);
            const linear = elapsed <= remaining ? before.value + difference * t : after.value - difference * u;
            const value = linear + before.tension * t * u * ((startDelta - difference) * u + (difference - endDelta) * t);
            return Math.max(Math.min(before.value, after.value), Math.min(Math.max(before.value, after.value), value));
        };
        const derivative = (seconds: number) => {
            if (single || before.interpolation === "hold" || !Number.isFinite(length)) return 0;
            const { t, u } = ratios(seconds);
            const automatic = startDelta * (u * u - 2 * t * u) + endDelta * (t * t - 2 * t * u) + 6 * difference * t * u;
            return (difference + before.tension * (automatic - difference)) / length;
        };
        // A single nonempty point covers the circle; viewport clipping only
        // limits the path, not the coverage used to sample its right edge.
        const intervals = single ? [[-Infinity, Infinity]] : beforeTime < afterTime
            ? [[beforeTime, afterTime]]
            : compareAutomationTime(before, after) < 0 ? []
                : [[beforeTime, afterTime + curve.cycleSeconds], [beforeTime - curve.cycleSeconds, afterTime]];
        for (const [start, finish] of intervals) {
            const a = Math.max(begin, start), b = Math.min(end, finish);
            if (!(a < b)) continue;
            const first = sample(a), last = before.interpolation === "hold" ? before.value : sample(b);
            // At the next point, an incoming endpoint precedes any outgoing
            // jump. Use its left-hand limit for this segment's terminal value.
            const terminal = !single && b === afterTime && before.interpolation === "bezier" ? after.value : last;
            paths.push({ begin: { seconds: a, value: first }, end: { seconds: b, value: terminal },
                first: first + derivative(a) * (b - a) / 3, second: terminal - derivative(b) * (b - a) / 3 });
            sampleSegments.push({ start, finish, sample });
        }
    }
    paths.sort((a, b) => a.begin.seconds - b.begin.seconds);
    const visible = points.filter((point) => time(point) >= begin && time(point) <= end);
    const valueAt = (seconds: number) => sampleSegments.find((segment) => seconds >= segment.start && seconds < segment.finish)?.sample(seconds);
    const vertices: Array<{ seconds: number; value: number }> = [];
    const first = valueAt(begin), last = valueAt(end);
    if (first !== undefined) vertices.push({ seconds: begin, value: first });
    for (const point of visible) vertices.push({ seconds: time(point), value: clampAutomationValue(point.value, bounds) });
    if (last !== undefined) vertices.push({ seconds: end, value: last });
    return { points: visible, paths, vertices };
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

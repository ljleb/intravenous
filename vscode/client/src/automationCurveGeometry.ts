export type CurveInterpolation = "empty" | "hold" | "bezier";
export type AutomationPointPosition = { id: number; seconds: number; value: number };
export type AutomationPoint = AutomationPointPosition & { interpolation: CurveInterpolation; tension: number };
export type AutomationCurve = { points: AutomationPoint[]; cycleSeconds: number };
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

// Only the viewport and its neighboring point groups are visited. Extra end
// groups supply tangents across the cyclic seam; drafts replace point identity.
export function automationWindow(curve: AutomationCurve, begin: number, end: number,
    drafts: readonly AutomationPointPosition[] = []) {
    const boundary = (seconds: number, after: boolean) => {
        let first = 0, last = curve.points.length;
        while (first < last) {
            const middle = first + Math.floor((last - first) / 2);
            if (curve.points[middle].seconds < seconds || (after && curve.points[middle].seconds === seconds)) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const neighbors = drafts.length + 2;
    const collect = (start: number, finish: number) => {
        for (let i = 0; i < neighbors && start > 0; ++i) start = boundary(curve.points[start - 1].seconds, false);
        for (let i = 0; i < neighbors && finish < curve.points.length; ++i) finish = boundary(curve.points[finish].seconds, true);
        return curve.points.slice(start, finish);
    };
    const selected = new Map(collect(boundary(begin, false), boundary(end, true)).map((point) => [point.id, point]));
    for (const point of [...collect(0, 0), ...collect(curve.points.length, curve.points.length)]) selected.set(point.id, point);
    // Draft identities are indexed once per accepted curve, rather than scanned
    // through the complete point list on every pointer movement.
    const byId = automationCurveBounds(curve).order;
    for (const draft of drafts) {
        const original = curve.points[byId.get(draft.id) ?? -1];
        if (original) selected.set(draft.id, { ...original, ...draft });
    }
    const points = [...selected.values()].sort((a, b) => a.seconds - b.seconds || a.id - b.id);
    const groups: Array<{ first: AutomationPoint; last: AutomationPoint }> = [];
    for (const point of points) {
        const last = groups.at(-1);
        if (last?.first.seconds === point.seconds) last.last = point;
        else groups.push({ first: point, last: point });
    }
    const forward = (from: number, to: number) => to >= from ? to - from : to - from + curve.cycleSeconds;
    const paths: CurvePath[] = [];
    const sampleSegments: Array<{ start: number; finish: number; sample: (seconds: number) => number }> = [];
    for (let i = 0; i < groups.length; ++i) {
        const group = groups[i], next = groups[(i + 1) % groups.length];
        const before = group.last, after = next.first;
        if (before.interpolation === "empty") continue;
        const single = groups.length === 1;
        const length = single ? curve.cycleSeconds : forward(before.seconds, after.seconds);
        let startDelta = 0, endDelta = 0;
        const difference = single ? 0 : after.value - before.value;
        if (!single && before.interpolation === "bezier" && before.tension > 0 && Number.isFinite(length)) {
            const previous = groups[(i + groups.length - 1) % groups.length].last;
            const following = groups[(i + 2) % groups.length].first;
            const slope = difference / length;
            const previousLength = forward(previous.seconds, before.seconds);
            const nextLength = forward(after.seconds, following.seconds);
            if (previous.interpolation === "bezier" && group.first.value === before.value && Number.isFinite(previousLength))
                startDelta = curveTangent((before.value - previous.value) / previousLength, slope, previousLength, length) * length;
            if (next.last.interpolation === "bezier" && after.value === next.last.value && Number.isFinite(nextLength))
                endDelta = curveTangent(slope, (following.value - after.value) / nextLength, length, nextLength) * length;
        }
        const ratios = (seconds: number) => {
            const elapsed = forward(before.seconds, seconds), remaining = forward(seconds, after.seconds);
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
        const intervals = single ? [[-Infinity, Infinity]] : before.seconds < after.seconds
            ? [[before.seconds, after.seconds]]
            : [[before.seconds, after.seconds + curve.cycleSeconds], [before.seconds - curve.cycleSeconds, after.seconds]];
        for (const [start, finish] of intervals) {
            const a = Math.max(begin, start), b = Math.min(end, finish);
            if (!(a < b)) continue;
            const first = sample(a), last = before.interpolation === "hold" ? before.value : sample(b);
            // At the next point, an incoming endpoint precedes any outgoing
            // jump. Use its left-hand limit for this segment's terminal value.
            const terminal = !single && b === after.seconds && before.interpolation === "bezier" ? after.value : last;
            paths.push({ begin: { seconds: a, value: first }, end: { seconds: b, value: terminal },
                first: first + derivative(a) * (b - a) / 3, second: terminal - derivative(b) * (b - a) / 3 });
            sampleSegments.push({ start, finish, sample });
        }
    }
    paths.sort((a, b) => a.begin.seconds - b.begin.seconds);
    const visible = points.filter((point) => point.seconds >= begin && point.seconds <= end);
    const valueAt = (seconds: number) => sampleSegments.find((segment) => seconds >= segment.start && seconds < segment.finish)?.sample(seconds);
    const vertices: Array<{ seconds: number; value: number }> = [];
    const first = valueAt(begin), last = valueAt(end);
    if (first !== undefined) vertices.push({ seconds: begin, value: first });
    for (const { seconds, value } of visible) vertices.push({ seconds, value });
    if (last !== undefined) vertices.push({ seconds: end, value: last });
    return { points: visible, paths, vertices };
}

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

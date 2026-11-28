import { waveformPoint, waveformTracksForSpans, type WaveformPoint } from "./observationWaveform";
import { presentationObservationSpans } from "./presentationObservationSpans";
import { observationSampleOffsets } from "./sampleWaveform";
import { timelineCurvePointBytes, type PresentationFrame, type PresentationMark, type PresentationSnapshot,
    type PresentationTarget, type PresentationTimelineCurve } from "./presentationPlugin";
import { timelineObservationOffset, timelineObservationSpan, timelineSampleCount, timelineSamples, type TimelineSamples } from "./timelineSamples";

type CurvePoint = { sample: bigint; value: number; tension: number; startDelta: number; endDelta: number;
    target: number; mark: number; tensionTarget: number; tensionMark: number; interpolation: number;
    selected: boolean; active: boolean };
type Group = { sample: bigint; points: CurvePoint[] };
type Arc = { begin: number; end: number; valueAt: (sample: number) => number };

const retained = new WeakMap<ArrayBuffer, Group[]>();
function curveGroups(points: ArrayBuffer): Group[] {
    const previous = retained.get(points);
    if (previous) return previous;
    const view = new DataView(points);
    const groups: Group[] = [];
    for (let at = 0; at < view.byteLength; at += timelineCurvePointBytes) {
        const sample = view.getBigInt64(at, true), flags = view.getUint32(at + 52, true);
        let group = groups.at(-1);
        if (group?.sample !== sample) {
            group = { sample, points: [] };
            groups.push(group);
        }
        group.points.push({ sample, startDelta: view.getFloat64(at + 8, true), endDelta: view.getFloat64(at + 16, true),
            value: view.getFloat32(at + 24, true), tension: view.getFloat32(at + 28, true), target: view.getUint32(at + 32, true),
            mark: view.getUint32(at + 36, true), tensionTarget: view.getUint32(at + 40, true),
            tensionMark: view.getUint32(at + 44, true), interpolation: view.getUint32(at + 48, true),
            selected: !!(flags & 1), active: !!(flags & 2) });
    }
    retained.set(points, groups);
    return groups;
}

function lowerBound<T>(items: readonly T[], before: (item: T) => boolean): number {
    let start = 0, end = items.length;
    while (start < end) {
        const middle = start + ((end - start) >> 1);
        if (before(items[middle])) start = middle + 1;
        else end = middle;
    }
    return start;
}

function visibleCurve(groups: Group[], { origin, span }: TimelineSamples) {
    const arcs: Arc[] = [];
    const points: Array<{ offset: number; point: CurvePoint }> = [];
    const count = groups.length;
    if (!count) return { arcs, points };
    const first = lowerBound(groups, ({ sample }) => sample < origin.index || (sample === origin.index && origin.fraction > 0));
    // Start with the segment containing the left edge. For a full lap, its
    // other end can also be visible at the right edge.
    for (let step = -1; step < count; ++step) {
        const index = (first + step + count) % count;
        const group = groups[index], next = groups[(index + 1) % count];
        const forward = BigInt.asUintN(64, group.sample - origin.index);
        const sameSample = forward === 0n && origin.fraction > 0;
        const begin = step === -1 ? (sameSample ? 0n : forward - timelineSampleCount)
            : sameSample ? timelineSampleCount : forward;
        const offset = Number(begin) - origin.fraction;
        if (offset > span) break;
        if (step >= 0 && offset >= 0) for (const point of group.points) points.push({ offset, point });
        const firstPoint = group.points.at(-1)!;
        if (offset >= span || firstPoint.interpolation === 0) continue;
        const length = count === 1 ? timelineSampleCount : BigInt.asUintN(64, next.sample - group.sample);
        // Add on the exact sample lattice before converting the endpoint.
        // A huge elapsed distance can otherwise swallow a one-sample tail.
        const end = Number(begin + length) - origin.fraction;
        if (end <= 0 || end <= offset) continue;
        const last = next.points[0].value, firstValue = firstPoint.value;
        const difference = last - firstValue, duration = Number(length);
        const valueAt = count === 1 || firstPoint.interpolation === 1 ? () => firstValue : (sample: number) => {
            const t = (sample - offset) / duration, u = (end - sample) / duration;
            const linear = t <= u ? firstValue + difference * t : last - difference * u;
            const value = linear + firstPoint.tension * t * u
                * ((firstPoint.startDelta - difference) * u + (difference - firstPoint.endDelta) * t);
            return clamp(value, Math.min(firstValue, last), Math.max(firstValue, last));
        };
        arcs.push({ begin: offset, end, valueAt });
    }
    return { arcs, points };
}

function clamp(value: number, low: number, high: number) {
    return Math.min(high, Math.max(low, value));
}

function axisFraction(curve: PresentationTimelineCurve, value: number) {
    const { cropped, pan, center, zoom } = curve;
    if (cropped) return .5 + (value - pan) / (2 * Math.max(Math.abs(pan) * 2 ** -23, 1 / zoom));
    const fraction = .5 + Math.atan((value - pan) * Math.min(1, zoom)) / Math.PI;
    return zoom > 1 ? .5 + (fraction - center) * zoom : fraction;
}

function interpolate(samples: readonly WaveformPoint[], sample: number) {
    const at = lowerBound(samples, ({ offset }) => offset < sample);
    if (at === 0) return samples[0].value;
    if (at === samples.length) return samples[at - 1].value;
    const left = samples[at - 1], right = samples[at];
    const mix = (sample - left.offset) / (right.offset - left.offset);
    return (1 - mix) * left.value + mix * right.value;
}

type Interval = { begin: number; end: number };

function coverage(intervals: Interval[], edges: number[]) {
    intervals.sort((a, b) => a.begin - b.begin);
    for (const { begin, end } of intervals) edges.push(begin, end);
    let next = 0, end = -Infinity;
    return (sample: number) => {
        while (next < intervals.length && intervals[next].begin <= sample) end = Math.max(end, intervals[next++].end);
        return end > sample;
    };
}

function bound(snapshot: PresentationSnapshot, id: string, view: TimelineSamples, minimum: boolean, edges: number[]) {
    const port = snapshot.node.ports.find(({ name, direction }) => name === id && direction === "input");
    if (!id || !port) return;
    if (!port.connected && Number.isFinite(port.manualValue)) return { advance: () => true, valueAt: () => port.manualValue, hasValues: true };
    if (!port.connected) return;
    const observation = snapshot.observations[id];
    const spans = observation ? presentationObservationSpans(observation, timelineObservationSpan(view)) : [];
    const regions = waveformTracksForSpans(spans, view, waveformPoint).sort((a, b) => a.begin - b.begin);
    const visibleOffset = timelineObservationOffset(view);
    const complete: Interval[] = [], covered: Interval[] = [];
    for (const span of spans) {
        if (span.result.status !== "ready" || span.result.truncated) continue;
        const begin = Math.max(0, Number(span.begin) + visibleOffset);
        const end = Math.min(view.span, Number(span.end) + visibleOffset);
        if (begin >= end) continue;
        complete.push({ begin, end });
        const lattice = observationSampleOffsets(span.result, span.windowOffset);
        for (const region of span.result.regions) {
            const left = lattice.at(region.begin) + visibleOffset;
            const start = Math.max(begin, left), stop = Math.min(end, left + Number(region.length) * lattice.scale);
            if (start < stop) covered.push({ begin: start, end: stop });
        }
    }
    const completeAt = coverage(complete, edges), coveredAt = coverage(covered, edges);
    for (const { begin, end, samples } of regions) {
        edges.push(begin, end);
        const first = lowerBound(samples, ({ offset }) => offset <= begin);
        for (let i = first; i < samples.length && samples[i].offset < end; ++i) edges.push(samples[i].offset);
    }
    let next = 0;
    let active: typeof regions = [];
    return {
        hasValues: regions.length > 0,
        advance(sample: number) {
            active = active.filter((region) => region.end > sample);
            while (next < regions.length && regions[next].begin <= sample) {
                const region = regions[next++];
                if (region.end > sample) active.push(region);
            }
            return active.length > 0 || (completeAt(sample) && !coveredAt(sample));
        },
        valueAt(sample: number) {
            let value: number | undefined;
            for (const { samples } of active) {
                const candidate = interpolate(samples, sample);
                value = value === undefined ? candidate : minimum ? Math.max(candidate, value) : Math.min(candidate, value);
            }
            return value;
        },
    };
}

class Track {
    private readonly runs: number[][] = [];
    private run: number[] | undefined;

    append(x: number, y: number, first: boolean) {
        const run = this.run;
        if (first && run && run.at(-2) === x && run.at(-1) === y) return;
        if (first || !run) this.runs.push(this.run = []);
        this.run!.push(x, y);
    }

    marks(id: number, ink: PresentationMark["ink"], opacity: number, strokeWidth: number, dashed?: true): PresentationMark[] {
        const count = Math.min(256, this.runs.length);
        const runs = Array.from({ length: count }, (_, i) => this.runs[spread(i, count, this.runs.length)]);
        const interior = runs.reduce((sum, run) => sum + Math.max(0, run.length / 2 - 2), 0);
        let seen = 0, spent = 0;
        return runs.flatMap((source, i): PresentationMark[] => {
            const length = source.length / 2;
            if (length < 2) return [];
            seen += length - 2;
            const total = interior ? Math.min(seen, Math.floor(seen * (2400 - 2 * count) / interior)) : 0;
            const keep = 2 + total - spent;
            spent = total;
            const coordinates = new Float64Array(keep * 2);
            for (let j = 0; j < keep; ++j) {
                const at = Math.round(j * (length - 1) / (keep - 1)) * 2;
                coordinates[2 * j] = source[at]; coordinates[2 * j + 1] = source[at + 1];
            }
            return [{ type: "packedPolyline", id: String(id + i), coordinates, ink, opacity, strokeWidth, dashed }];
        });
    }
}

const spread = (index: number, count: number, total: number) => count <= 1 ? 0 : Math.floor(index * (total - 1) / (count - 1));

function curveMarks(arcs: Arc[], snapshot: PresentationSnapshot, curve: PresentationTimelineCurve, view: TimelineSamples,
    xAt: (sample: number) => number, yAt: (value: number) => number): PresentationMark[] {
    const edges = [0, view.span];
    for (const { begin, end } of arcs) edges.push(Math.max(0, begin), Math.min(view.span, end));
    const minimum = bound(snapshot, curve.minimumInput, view, true, edges);
    const maximum = bound(snapshot, curve.maximumInput, view, false, edges);
    const bounded = minimum?.hasValues || maximum?.hasValues;
    const authored = new Track(), output = new Track(), lower = new Track(), upper = new Track();
    const pixels = clamp(Math.ceil(snapshot.viewport.width), 1, 4096);
    edges.sort((a, b) => a - b);
    let arcIndex = 0;
    for (let i = 1; i < edges.length; ++i) {
        const begin = edges[i - 1], end = edges[i];
        if (begin >= end) continue;
        while (arcIndex < arcs.length && arcs[arcIndex].end <= begin) ++arcIndex;
        const nextArc = arcs[arcIndex];
        const arc = nextArc && nextArc.begin <= begin ? nextArc : undefined;
        const knowMinimum = minimum?.advance(begin) ?? true, knowMaximum = maximum?.advance(begin) ?? true;
        if (!arc && !bounded) continue;
        const steps = Math.max(1, Math.ceil((end - begin) / view.span * pixels));
        for (let j = 0; j <= steps; ++j) {
            const sample = j === steps ? end : begin + (end - begin) * (j / steps), first = j === 0;
            const x = xAt(sample);
            const low = minimum?.valueAt(sample), high = maximum?.valueAt(sample);
            if (low !== undefined) lower.append(x, yAt(low), first);
            if (high !== undefined) upper.append(x, yAt(high), first);
            if (!arc) continue;
            const value = arc.valueAt(sample);
            authored.append(x, yAt(value), first);
            // A known gap supplies no limit. A bound that is still loading leaves the output unknown.
            if (bounded && knowMinimum && knowMaximum) {
                const clamped = low !== undefined && high !== undefined && low > high ? .5 * low + .5 * high
                    : clamp(value, low ?? -Infinity, high ?? Infinity);
                output.append(x, yAt(clamped), first);
            }
        }
    }
    return [...authored.marks(1001, "input", bounded ? .32 : 1, 1.5), ...output.marks(1301, "input", 1, 1.5),
        ...lower.marks(1601, "highlight", .55, 1, true), ...upper.marks(1901, "warning", .55, 1, true)];
}

function pointMarks(curve: PresentationTimelineCurve, points: ReturnType<typeof visibleCurve>["points"],
    width: number, height: number, xAt: (sample: number) => number) {
    const marks: PresentationMark[] = [];
    const contents = { x: 0, y: 0, width, height, drag: "absolute" } as const;
    const targets: PresentationTarget[] = [
        { ...contents, id: String(curve.selectionTarget), hint: "Drag to select · Right-drag to add" },
        { ...contents, id: String(curve.additionTarget), drag: "relative", button: "secondary", wheel: true,
            hideCursor: true, cursorReturn: true },
    ];
    const handles: PresentationTarget[] = [], dots: PresentationTarget[] = [];
    const visible = points.flatMap(({ offset, point }) => {
        const fraction = axisFraction(curve, point.value);
        return fraction >= 0 && fraction <= 1 ? [{ x: xAt(offset), y: clamp((1 - fraction) * height, 2, Math.max(2, height - 2)), point }] : [];
    });
    const count = Math.min(490, visible.length);
    const sampled = new Set(Array.from({ length: count }, (_, i) => spread(i, count, visible.length)));
    const active = visible.findIndex(({ point }) => point.active);
    if (active >= 0) sampled.add(active);
    const box = (x: number, y: number, radius: number) => ({ x: x - radius, y: y - radius, width: 2 * radius, height: 2 * radius });
    const decimal = (value: number, digits: number) => String(Number(value.toFixed(digits)));
    for (const index of sampled) {
        const { x, y, point } = visible[index];
        const target = String(point.target), mark = point.mark;
        dots.push({ ...box(x, y, 8), id: target, drag: "relative", context: true, hideCursor: true, wheel: true,
            cursorReturn: true, hint: `${point.sample} smp · Value ${decimal(point.value, 5)} · Tension ${decimal(point.tension, 2)}`,
            shortcuts: "⇧Drag Keep type · ⇧Click Delete · Right-click Segment" });
        if (point.selected) marks.push({ type: "circle", id: String(mark), x, y, radius: 5.5, ink: "background", border: "selection" });
        marks.push({ type: "circle", id: String(mark + 1), x, y, radius: point.selected ? 3.2 : 2.6, ink: "input", target });
        if (!point.tensionTarget) continue;
        const handle = y + (y < height / 2 ? 15 : -15), id = String(point.tensionTarget);
        handles.push({ ...box(x, handle, 6), id, drag: "relative", context: true, hideCursor: true, cursor: "ew-resize",
            hint: `Tension ${decimal(point.tension, 2)}` });
        marks.push({ type: "polyline", id: String(point.tensionMark), ink: "input", opacity: .22, points: [{ x, y }, { x, y: handle }] },
            { type: "polyline", id: String(point.tensionMark + 1), ink: "input", opacity: .25 + point.tension * .75,
                target: id, fill: true, points: [{ x, y: handle - 3.4 }, { x: x + 3.4, y: handle },
                    { x, y: handle + 3.4 }, { x: x - 3.4, y: handle }] });
    }
    return { marks, targets: [...targets, ...handles, ...dots] };
}

export function projectTimelineCurve(curve: PresentationTimelineCurve, snapshot: PresentationSnapshot):
    { marks: PresentationMark[]; targets: PresentationTarget[] } | undefined {
    const { time, width, height } = snapshot.viewport;
    if (!time || !Number.isFinite(time.spanSamples) || time.spanSamples <= 0 || time.spanSamples > Number(timelineSampleCount)
        || !Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) return;
    const view = timelineSamples(time);
    const { arcs, points } = visibleCurve(curveGroups(curve.points), view);
    const x = (samples: number) => samples / view.span * width;
    const y = (value: number) => clamp(1 - axisFraction(curve, value), -1, 2) * height;
    const marks = curveMarks(arcs, snapshot, curve, view, x, y);
    const editing = curve.editable && snapshot.editable ? pointMarks(curve, points, width, height, x) : undefined;
    if (editing) marks.push(...editing.marks);
    return { marks, targets: editing?.targets ?? [] };
}

export function projectTimelineCurveFrame(frame: PresentationFrame, snapshot: PresentationSnapshot): PresentationFrame {
    const { timelineCurve, ...pixelFrame } = frame;
    const curve = timelineCurve && projectTimelineCurve(timelineCurve, snapshot);
    if (!curve) return pixelFrame;
    return { ...pixelFrame, marks: [...pixelFrame.marks ?? [], ...curve.marks], targets: [...pixelFrame.targets ?? [], ...curve.targets] };
}

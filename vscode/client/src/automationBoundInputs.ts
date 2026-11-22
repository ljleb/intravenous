import { clampAutomationValue, type AutomationValueBounds, type CurvePath } from "./automationCurveGeometry";
import type { ObservationSpan } from "./observationWindows";
import type { ObservationWindowResult } from "./observations";
import { observationSampleOffsets } from "./sampleWaveform";
import { timelineObservationOffset, type TimelineSamples } from "./timelineSamples";

export type AutomationBoundInput = "min" | "max";

export type BoundSample = { seconds: number; value: number };
type BoundTrack = { begin: number; end: number; samples: BoundSample[] };
export const automationBoundVisiblePositions = 1024;

export function automationBoundTracksForSpans(spans: readonly ObservationSpan[], view: TimelineSamples, sampleRate: number): BoundTrack[] {
    const tracks: BoundTrack[] = [];
    const visibleOffset = timelineObservationOffset(view);
    const projected = new Map<ObservationWindowResult, { offset: bigint; tracks: BoundTrack[] }>();
    for (const span of spans) {
        const { result } = span;
        const begin = Math.max(0, (Number(span.begin) + visibleOffset) / sampleRate);
        const end = Math.min(view.span, Number(span.end) + visibleOffset) / sampleRate;
        let source = projected.get(result);
        if (!source || source.offset !== span.windowOffset) {
            source = { offset: span.windowOffset,
                tracks: automationBoundTracks(result, span.windowOffset, sampleRate, visibleOffset) };
            projected.set(result, source);
        }
        for (const track of source.tracks) {
            const clippedBegin = Math.max(begin, track.begin), clippedEnd = Math.min(end, track.end);
            if (clippedBegin < clippedEnd) tracks.push({ begin: clippedBegin, end: clippedEnd, samples: track.samples });
        }
    }
    return tracks;
}

// Use the project timeline for X. The observation can count samples at a
// different pace, and its Region and sample indices can cross the circle seam.
export function automationBoundTracks(result: ObservationWindowResult | undefined,
    windowOffset: bigint, sampleRate: number, visibleOffset = 0): BoundTrack[] {
    if (!result || result.status !== "ready") return [];
    const lattice = observationSampleOffsets(result, windowOffset);
    const secondsPerSample = lattice.scale / sampleRate;
    const time = (index: string) => (visibleOffset + lattice.at(index)) / sampleRate;
    const tracks: BoundTrack[] = [];
    for (const region of result.regions) {
        let begin = time(region.begin);
        const end = begin + Number(region.length) * secondsPerSample;
        let samples: BoundSample[] = [];
        let hasGap = false;
        for (const point of region.points) {
            const value = point.value?.value;
            if (!point.available || typeof value !== "number" || !Number.isFinite(value)) {
                const last = samples.at(-1);
                if (last) tracks.push({ begin, end: Math.min(end, last.seconds + secondsPerSample), samples });
                samples = [];
                hasGap = true;
                continue;
            }
            const seconds = time(point.index);
            if (hasGap && !samples.length) begin = seconds;
            samples.push({ seconds, value });
        }
        if (samples.length) tracks.push({ begin, end, samples });
    }
    return tracks;
}

function representativeBoundAt(tracks: Iterable<BoundTrack>, seconds: number, side: AutomationBoundInput): number | undefined {
    let bound: number | undefined;
    const combine = side === "min" ? Math.max : Math.min;
    for (const { samples: track } of tracks) {
        let low = 0, high = track.length - 1;
        while (high - low > 1) {
            const middle = low + Math.floor((high - low) / 2);
            if (track[middle].seconds > seconds) high = middle;
            else low = middle;
        }
        const left = track[low], right = track[high];
        const fraction = seconds <= left.seconds ? 0 : seconds >= right.seconds ? 1
            : (seconds - left.seconds) / (right.seconds - left.seconds);
        const value = (1 - fraction) * left.value + fraction * right.value;
        bound = bound === undefined ? value : combine(bound, value);
    }
    return bound;
}

function activeBoundTracks(tracks: readonly BoundTrack[]) {
    const byBegin = tracks.slice().sort((a, b) => a.begin - b.begin);
    const byEnd = tracks.slice().sort((a, b) => a.end - b.end);
    const active = new Set<BoundTrack>();
    let nextBegin = 0, nextEnd = 0;
    return (seconds: number) => {
        while (nextBegin < byBegin.length && byBegin[nextBegin].begin <= seconds) active.add(byBegin[nextBegin++]);
        while (nextEnd < byEnd.length && byEnd[nextEnd].end <= seconds) active.delete(byEnd[nextEnd++]);
        return active;
    };
}

const previewTracks = ["authored", "output", "min", "max"] as const;
type AutomationPreview = Record<typeof previewTracks[number], BoundSample[][]>;
type AutomationPreviewSample = { seconds: number; authored?: number; output?: number } & AutomationValueBounds;

export function sampleAutomationPreview(paths: readonly CurvePath[], lower: readonly BoundTrack[],
    upper: readonly BoundTrack[], manual: AutomationValueBounds, span: number, width: number): AutomationPreview {
    const preview: AutomationPreview = { authored: [], output: [], min: [], max: [] };
    if (!(span > 0) || !Number.isFinite(span)) return preview;

    // Split at Coverage edges before sampling. The two sides of an edge can
    // use different bounds even though they share a time coordinate.
    const edges = new Set([0, span]);
    const add = (seconds: number) => { if (seconds > 0 && seconds < span) edges.add(seconds); };
    for (const path of paths) {
        add(path.begin.seconds);
        add(path.end.seconds);
    }
    for (const track of [...lower, ...upper]) {
        add(track.begin);
        add(track.end);
        for (const sample of track.samples)
            if (track.begin < sample.seconds && sample.seconds < track.end) add(sample.seconds);
    }
    const times = [...edges].sort((a, b) => a - b);
    const pixelCount = Math.max(1, Math.min(4096, Math.ceil(width)));
    const lowerAt = activeBoundTracks(lower), upperAt = activeBoundTracks(upper);
    let curveIndex = 0;
    for (let i = 1; i < times.length; ++i) {
        const begin = times[i - 1], end = times[i];
        const minTracks = lowerAt(begin), maxTracks = upperAt(begin);
        while (curveIndex < paths.length && paths[curveIndex].end.seconds <= begin) ++curveIndex;
        const candidate = paths[curveIndex];
        const curve = candidate && candidate.begin.seconds <= begin ? candidate : undefined;
        if (!curve && !minTracks.size && !maxTracks.size && manual.min === undefined && manual.max === undefined) continue;
        const steps = Math.max(1, Math.ceil((end - begin) / span * pixelCount));
        const samples: AutomationPreviewSample[] = [];
        for (let step = 0; step <= steps; ++step) {
            const seconds = step === steps ? end : begin + (end - begin) * (step / steps);
            const min = manual.min ?? representativeBoundAt(minTracks, seconds, "min");
            const max = manual.max ?? representativeBoundAt(maxTracks, seconds, "max");
            let authored: number | undefined;
            if (curve) {
                const t = (seconds - curve.begin.seconds) / (curve.end.seconds - curve.begin.seconds);
                const u = 1 - t;
                authored = u ** 3 * curve.begin.value + 3 * u ** 2 * t * curve.first
                    + 3 * u * t ** 2 * curve.second + t ** 3 * curve.end.value;
            }
            samples.push({ seconds, min, max, authored,
                output: authored === undefined ? undefined : clampAutomationValue(authored, { min, max }) });
        }
        for (const name of previewTracks) {
            const firstValue = samples[0][name];
            if (firstValue === undefined) continue;
            let run = preview[name].at(-1);
            const last = run?.at(-1);
            const continues = last?.seconds === begin && last.value === firstValue;
            if (!continues) preview[name].push(run = []);
            for (let j = continues ? 1 : 0; j < samples.length; ++j)
                run!.push({ seconds: samples[j].seconds, value: samples[j][name]! });
        }
    }
    return preview;
}

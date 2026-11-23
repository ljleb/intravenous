import type { ObservationSpan } from "./observationWindows";
import type { ObservationWindowResult } from "./observations";
import { observationSampleOffsets } from "./sampleWaveform";
import { timelineObservationOffset, type TimelineSamples } from "./timelineSamples";

export type WaveformPoint = { index: string; offset: number; value: number };
export type WaveformTrack<Point> = { begin: number; end: number; paceExponent: number; samples: Point[] };
export const waveformPoint = (index: string, offset: number, value: number): WaveformPoint => ({ index, offset, value });
type ProjectPoint<Point> = (index: string, offset: number, value: number) => Point;

// Offsets are in project samples from the viewport origin. The original
// index remains exact on the observed port's own sample lattice.
export function waveformTracks<Point>(result: ObservationWindowResult | undefined,
    windowOffset: bigint, visibleOffset: number, project: ProjectPoint<Point>): WaveformTrack<Point>[] {
    if (!result || result.status !== "ready") return [];
    const lattice = observationSampleOffsets(result, windowOffset);
    const at = (index: string) => visibleOffset + lattice.at(index);
    const paceExponent = result.paceExponent ?? 0;
    const tracks: WaveformTrack<Point>[] = [];
    for (const region of result.regions) {
        let begin = at(region.begin);
        const end = begin + Number(region.length) * lattice.scale;
        let samples: Point[] = [];
        let lastOffset = begin;
        let gap = false;
        for (const point of region.points) {
            const value = point.value?.value;
            if (!point.available || typeof value !== "number" || !Number.isFinite(value)) {
                if (samples.length) tracks.push({ begin, end: Math.min(end, lastOffset + lattice.scale), paceExponent, samples });
                samples = [];
                gap = true;
                continue;
            }
            const offset = at(point.index);
            if (gap && !samples.length) begin = offset;
            samples.push(project(point.index, offset, value));
            lastOffset = offset;
        }
        if (samples.length) tracks.push({ begin, end, paceExponent, samples });
    }
    return tracks;
}

export function waveformTracksForSpans<Point>(spans: Iterable<ObservationSpan>, view: TimelineSamples,
    project: ProjectPoint<Point>): WaveformTrack<Point>[] {
    const tracks: WaveformTrack<Point>[] = [];
    const visibleOffset = timelineObservationOffset(view);
    const projected = new Map<ObservationWindowResult, { offset: bigint; tracks: WaveformTrack<Point>[] }>();
    for (const span of spans) {
        const { result } = span;
        const begin = Math.max(0, Number(span.begin) + visibleOffset);
        const end = Math.min(view.span, Number(span.end) + visibleOffset);
        let source = projected.get(result);
        if (!source || source.offset !== span.windowOffset) {
            source = { offset: span.windowOffset, tracks: waveformTracks(result, span.windowOffset, visibleOffset, project) };
            projected.set(result, source);
        }
        for (const track of source.tracks) {
            const clippedBegin = Math.max(begin, track.begin), clippedEnd = Math.min(end, track.end);
            if (clippedBegin < clippedEnd) tracks.push({ ...track, begin: clippedBegin, end: clippedEnd });
        }
    }
    return tracks;
}

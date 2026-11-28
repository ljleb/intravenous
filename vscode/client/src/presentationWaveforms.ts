import { waveformPoint, waveformTracksForSpans } from "./observationWaveform";
import type { PresentationObservation, PresentationSnapshot } from "./presentationPlugin";
import { presentationObservationSpans } from "./presentationObservationSpans";
import { timelineObservationSpan, timelineSamples, type PresentationTime } from "./timelineSamples";

export const waveformPointBytes = 24;
const maximumPoints = 2048;
const maximumRegions = 256;
export type WaveformRegion = { begin: number; end: number; paceExponent: number; points: ArrayBuffer };
export type PresentationWaveform = {
    id: string; regions: WaveformRegion[]; partial: boolean;
    status: PresentationObservation["status"]; error?: string;
};
const projections = new WeakMap<PresentationSnapshot["observations"], {
    time: PresentationTime; exclude: string; waveforms: PresentationWaveform[];
}>();

export function presentationWaveforms({ observations, viewport: { time } }: PresentationSnapshot,
    hostWaveforms: readonly string[] = []): PresentationWaveform[] {
    if (!time?.sampleRate) return [];
    const previous = projections.get(observations);
    const exclude = JSON.stringify(hostWaveforms);
    if (previous?.time === time && previous.exclude === exclude) return previous.waveforms;
    const view = timelineSamples(time);
    const window = timelineObservationSpan(view);
    const waveforms = Object.entries(observations).flatMap(([id, observation]) => {
        if (observation?.operation !== "waveform" || hostWaveforms.includes(id)) return [];
        const spans = presentationObservationSpans(observation, window);
        let remaining = maximumPoints;
        let partial = spans.some(({ result }) => result.truncated);
        const regions: WaveformRegion[] = [];
        for (const { begin, end, paceExponent, samples } of waveformTracksForSpans(spans, view, waveformPoint)) {
            // Keep the adjacent samples that interpolate into this retained
            // span, so replacing another part of the view leaves no seam.
            const start = Math.max(0, lowerBound(samples, begin) - 1);
            const stop = Math.min(samples.length, lowerBound(samples, end) + 1);
            const count = stop - start;
            if (regions.length >= maximumRegions || count > remaining) { partial = true; break; }
            const points = new ArrayBuffer(count * waveformPointBytes);
            const data = new DataView(points);
            for (let index = 0; index < count; ++index) {
                const sample = samples[start + index];
                const at = index * waveformPointBytes;
                data.setBigUint64(at, BigInt(sample.index), true);
                data.setFloat64(at + 8, sample.offset, true);
                data.setFloat64(at + 16, sample.value, true);
            }
            regions.push({ begin, end, paceExponent, points });
            remaining -= count;
        }
        return [{ id, regions, partial, status: observation.status, error: observation.error }];
    });
    projections.set(observations, { time, exclude, waveforms });
    return waveforms;
}

function lowerBound(samples: readonly { offset: number }[], value: number): number {
    let begin = 0, end = samples.length;
    while (begin < end) {
        const middle = begin + Math.floor((end - begin) / 2);
        if (samples[middle].offset < value) begin = middle + 1;
        else end = middle;
    }
    return begin;
}

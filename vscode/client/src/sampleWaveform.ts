import type { ObservationWindow, ObservationWindowResult } from "./observations";
import { timelineObservationOffset, timelineObservationSpan, timelineSamples, type PresentationTime, type TimelineSamples } from "./timelineSamples";

export function sampleWindow(time: PresentationTime, points: number): ObservationWindow | undefined {
    if (!time.sampleRate || !Number.isSafeInteger(time.sampleRate) || time.sampleRate < 1
        || !Number.isFinite(time.spanSamples) || time.spanSamples <= 0) return;
    return { ...timelineObservationSpan(timelineSamples(time)), points: Math.max(2, Math.min(1024, Math.ceil(Number.isFinite(points) ? points : 2))) };
}

// Observation Regions and points are expressed on the port's lattice. Anchor
// their forward distances at the individual RPC window before projecting X.
export function observationSampleOffsets(result: ObservationWindowResult, windowOffset = 0n) {
    const pace = result.paceExponent ?? 0;
    const rootBegin = BigInt.asUintN(64, BigInt(result.window.begin));
    const scale = 2 ** -pace;
    const nativeBegin = pace >= 0 ? rootBegin << BigInt(pace) : rootBegin >> BigInt(-pace);
    const bits = Math.min(64, 64 + pace);
    const distance = (index: string) => BigInt.asUintN(bits, BigInt(index) - nativeBegin);
    const at = pace >= 0 ? (index: string) => Number((windowOffset << BigInt(pace)) + distance(index)) * scale
        : (index: string) => Number(windowOffset + ((nativeBegin + distance(index)) << BigInt(-pace)) - rootBegin);
    return { scale, at };
}

export type SampleWaveformGeometry = { paths: string[]; coverage: Array<{ x: number; width: number }> };
// One path per actual logical Region, split at unavailable or nonfinite values.
// The default vertical scale is -1..1; specialized presentations can supply
// their existing bounds. This is representative sampling, not an exact envelope: connecting samples never promises to preserve intervening peaks.
export function sampleWaveform(result: ObservationWindowResult | undefined,
    bounds = { min: -1, max: 1 }, view?: TimelineSamples, windowOffset = 0n): SampleWaveformGeometry {
    const paths: string[] = [], coverage: SampleWaveformGeometry["coverage"] = [];
    if (result?.status !== "ready") return { paths, coverage };
    const span = view?.span ?? Number(result.window.length);
    if (!Number.isFinite(span) || span <= 0) return { paths, coverage };
    const offset = view ? timelineObservationOffset(view) : 0;
    const lattice = observationSampleOffsets(result, windowOffset);
    const pixelsPerSample = 1000 / span;
    const x = (index: string) => (offset + lattice.at(index)) * pixelsPerSample;
    for (const region of result.regions) {
        const begin = Math.max(0, x(region.begin));
        const end = Math.min(1000, x(region.begin) + Number(region.length) * lattice.scale * pixelsPerSample);
        if (end > begin) coverage.push({ x: begin, width: end - begin });
        let path = "";
        const finish = () => {
            if (path) paths.push(path.includes("L") ? path : `${path}L${path.slice(1)}`);
            path = "";
        };
        for (const point of region.points) {
            const value = point.value?.value;
            if (!point.available || typeof value !== "number" || !Number.isFinite(value)) {
                finish();
                continue;
            }
            const span = bounds.max - bounds.min;
            const normalized = span > 0 ? (value - bounds.min) / span : .5;
            const y = 95 - Math.max(0, Math.min(1, normalized)) * 90;
            path += `${path ? "L" : "M"}${x(point.index).toFixed(3)},${y.toFixed(3)}`;
        }
        finish();
    }
    return { paths, coverage };
}

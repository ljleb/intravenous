import type { ObservationTimeSpan, ObservationWindow, ObservationWindowResult } from "./observations";

export function sampleWindow(beginSeconds: number, endSeconds: number, sampleRate: number | null | undefined,
    points: number): ObservationWindow | undefined {
    if (!sampleRate || sampleRate <= 0 || !Number.isFinite(sampleRate)
        || !Number.isFinite(beginSeconds) || !Number.isFinite(endSeconds) || endSeconds <= beginSeconds) return;
    const begin = Math.floor(beginSeconds * sampleRate), end = Math.ceil(endSeconds * sampleRate);
    if (!Number.isSafeInteger(begin) || !Number.isSafeInteger(end) || !Number.isSafeInteger(end - begin) || end <= begin) return;
    return { begin: String(begin), length: String(end - begin),
        points: Math.max(2, Math.min(1024, Math.ceil(Number.isFinite(points) ? points : 2))) };
}

export type SampleWaveformGeometry = { paths: string[]; coverage: Array<{ x: number; width: number }> };
// One path per actual logical Region, split at unavailable or nonfinite values.
// The default vertical scale is -1..1; specialized presentations can supply
// their existing bounds. This is representative sampling, not an exact envelope: connecting samples never promises to preserve intervening peaks.
export function sampleWaveform(result: ObservationWindowResult | undefined,
    bounds = { min: -1, max: 1 }, visible?: ObservationTimeSpan): SampleWaveformGeometry {
    const paths: string[] = [], coverage: SampleWaveformGeometry["coverage"] = [];
    if (result?.status !== "ready") return { paths, coverage };
    const pace = result.paceExponent ?? 0;
    const window = visible ?? result.window;
    const rootBegin = BigInt(window.begin);
    const nativeBegin = pace >= 0 ? rootBegin << BigInt(pace) : rootBegin >> BigInt(-pace);
    const fraction = pace >= 0 ? 0 : Number(rootBegin - (nativeBegin << BigInt(-pace))) / 2 ** -pace;
    const nativeLength = Number(window.length) * 2 ** pace;
    if (!Number.isFinite(nativeLength) || nativeLength <= 0) return { paths, coverage };
    const x = (index: string) => (Number(BigInt.asIntN(64, BigInt(index) - nativeBegin)) - fraction) / nativeLength * 1000;
    for (const region of result.regions) {
        const begin = Math.max(0, x(region.begin));
        const end = Math.min(1000, x(region.begin) + Number(region.length) / nativeLength * 1000);
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

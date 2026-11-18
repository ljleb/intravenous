import { automationSampleOffset, clampAutomationValue, type AutomationValueBounds, type CurvePath } from "./automationCurveGeometry";
import type { ObservationWindowResult } from "./observations";

export type AutomationBoundInput = "min" | "max";

export function oppositeAutomationBound(definition: string | undefined, input: string): AutomationBoundInput | undefined {
    if (definition !== "iv.builtin.automation.curve") return;
    if (input === "min") return "max";
    if (input === "max") return "min";
}

export function pairedAutomationBoundLimits(opposite: AutomationBoundInput | undefined,
    control?: { enabled: boolean; value: number; liveValue?: number }): AutomationValueBounds {
    if (!opposite || !control?.enabled) return {};
    return { [opposite]: control.liveValue ?? control.value };
}

export type BoundSample = { seconds: number; value: number };
type BoundTrack = { begin: number; end: number; samples: BoundSample[] };

// Use the project timeline for X. The observation can count samples at a
// different pace, and its Region and sample indices can cross the circle seam.
export function automationBoundTracks(result: ObservationWindowResult | undefined,
    beginSeconds: number, sampleRate: number): BoundTrack[] {
    if (!result || result.status !== "ready" || result.outdated) return [];
    const pace = result.paceExponent ?? 0;
    const begin = BigInt(result.window.begin);
    const nativeBegin = pace >= 0 ? begin << BigInt(pace) : begin >> BigInt(-pace);
    const nativeFraction = pace >= 0 ? 0 : Number(begin - (nativeBegin << BigInt(-pace))) / 2 ** -pace;
    const offset = automationSampleOffset(begin, beginSeconds, sampleRate);
    const secondsPerSample = 1 / (2 ** pace * sampleRate);
    const time = (index: bigint) => offset
        + (Number(BigInt.asIntN(64, index - nativeBegin)) - nativeFraction) * secondsPerSample;
    const tracks: BoundTrack[] = [];
    for (const region of result.regions) {
        const first = BigInt(region.begin);
        const end = time(first + BigInt(region.length));
        let begin = time(first);
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
            const seconds = time(BigInt(point.index));
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
        for (const sample of track.samples) add(sample.seconds);
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

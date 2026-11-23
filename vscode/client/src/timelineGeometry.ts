import type { ObservationWindow } from "./observations";
import { observationViewportWindow } from "./observationWindows";
import { firstSignedSample, projectSampleRateFallback, signedTimelineSpans, timelineSampleExtent, timelineSampleCount, timelineSampleOffset,
    timelineSamples, type PresentationTime } from "./timelineSamples";

export type RulerTick = { key: number | string; x: number; label: string; sampleLabel?: string; minor?: boolean; seam?: boolean };
export const rulerMarkerPoints = "-5.5,0 5.5,0 0,7";
export const timelineSnapRadiusPx = 6;

function tickStep(target: number) {
    const exponent = Math.floor(Math.log10(target));
    const multiple = [1, 2, 5, 10].find((value) => value * 10 ** exponent >= target)!;
    return { multiple, exponent };
}

// Shared numeric graduation geometry. The domain owns its units and labels;
// timeline and meter renderers share tick placement and visual presentation.
export function numericTicks(begin: number, end: number, width: number,
    format: (value: number) => string = String, minimumSpacing = 90): Array<RulerTick & { value: number }> {
    const duration = end - begin;
    if (!(duration > 0) || !(width > 0) || !Number.isFinite(duration) || !Number.isFinite(width)) return [];
    const targetStep = duration / Math.max(1, Math.floor(width / minimumSpacing));
    const { multiple, exponent } = tickStep(targetStep);
    const step = multiple * 10 ** exponent;
    const first = Math.ceil(begin / step), last = Math.floor(end / step);
    const digits = Math.max(0, -Math.floor(Math.log10(step)));
    const ticks: Array<RulerTick & { value: number }> = [];
    const count = Math.min(1000, Math.max(0, last - first + 1));
    for (let offset = 0; offset < count; ++offset) {
        const raw = (first + offset) * step;
        if (ticks.length && ticks[ticks.length - 1].value === raw) continue;
        const value = Number(raw.toFixed(Math.min(12, digits)));
        if (ticks.length && ticks[ticks.length - 1].value === value) continue;
        ticks.push({ key: value, value, x: (raw - begin) / duration * width, label: format(value) });
    }
    return ticks;
}

export function timelineSampleFraction(index: bigint, time: PresentationTime): number {
    const view = timelineSamples(time);
    return timelineSampleOffset(view, index) / view.span;
}

export function timelineTicks(time: PresentationTime, width: number, showSampleLabels = false): Array<RulerTick & { seconds: number }> {
    const view = timelineSamples(time);
    if (!(view.span > 0 && width > 0) || !Number.isFinite(width) || !Number.isFinite(view.span)) return [];
    const rate = time.sampleRate ?? projectSampleRateFallback;
    const { multiple, exponent } = tickStep(view.span / rate / Math.max(1, Math.floor(width / 90)));
    const decimalPlaces = Math.max(0, -exponent);
    const denominator = 10n ** BigInt(decimalPlaces);
    const step = BigInt(multiple) * 10n ** BigInt(Math.max(0, exponent));
    const sampleStep = step * BigInt(rate);
    const parts = signedTimelineSpans(view.origin.index, timelineSampleExtent(view));
    const ticks: Array<RulerTick & { seconds: number }> = [];
    for (const part of parts) {
        const scaledBegin = part.begin * denominator;
        let tick = scaledBegin / sampleStep;
        if (tick * sampleStep < scaledBegin) ++tick;
        const last = (part.begin + part.length) * denominator;
        // The two signed ranges share a boundary in the viewport, with the
        // sample at that boundary belonging to the second range.
        for (let scaled = tick * sampleStep; scaled <= last && ticks.length < 1000; scaled += sampleStep, ++tick) {
            if (scaled === last && part.begin + part.length === -firstSignedSample) break;
            const distance = scaled - scaledBegin + part.offset * denominator;
            const offset = Number(distance) / Number(denominator) - view.origin.fraction;
            if (offset < 0 || offset > view.span || (offset === view.span && view.span === Number(timelineSampleCount))) continue;
            const seconds = Number(tick * step) / Number(denominator);
            const digits = (tick < 0n ? -tick * step : tick * step).toString().padStart(decimalPlaces + 1, "0");
            const whole = decimalPlaces ? digits.slice(0, -decimalPlaces) : digits;
            const fraction = decimalPlaces ? digits.slice(-decimalPlaces).replace(/0+$/, "") : "";
            const label = `${tick < 0n ? "-" : ""}${whole}${fraction ? "." + fraction : ""} s`;
            const magnitude = scaled < 0n ? -scaled : scaled;
            const sample = (magnitude + denominator / 2n) / denominator * (scaled < 0n ? -1n : 1n);
            ticks.push({ key: label, seconds, label, x: offset / view.span * width,
                sampleLabel: showSampleLabels && time.sampleRate ? `${BigInt.asIntN(64, sample)} smp` : undefined });
        }
    }
    return ticks;
}

export function timelineSeamX(time: PresentationTime, width: number): number | undefined {
    const view = timelineSamples(time);
    const seam = timelineSampleOffset(view, firstSignedSample);
    if (seam > 0 && seam < view.span) return seam / view.span * width;
}

export function timelineTicksWithSampleDivisions(time: PresentationTime, width: number): RulerTick[] {
    const major = timelineTicks(time, width, true);
    const view = timelineSamples(time);
    const ticks: RulerTick[] = [...major];
    const spacing = width / view.span;
    if (!time.sampleRate || !Number.isFinite(spacing) || spacing < 6) return ticks;
    const x = -view.origin.fraction * spacing;
    const count = Math.min(1000, Math.max(0, Math.floor((width - x) / spacing) + 1));
    for (let i = 0; i < count; ++i) {
        const position = x + i * spacing;
        if (position >= 0 && !major.some((tick) => Math.abs(tick.x - position) < 1))
            ticks.push({ key: `sample:${BigInt.asIntN(64, view.origin.index + BigInt(i))}`, x: position, label: "", minor: true });
    }
    return ticks.sort((a, b) => a.x - b.x);
}

export function timelineBeatObservationWindow(first: bigint, length: bigint, width: number,
    indexedEvents: boolean): ObservationWindow | undefined {
    if (length <= 0n || length > timelineSampleCount || !(width > 0) || !Number.isFinite(width)) return;
    return observationViewportWindow({
        begin: String(BigInt.asIntN(64, first)), length: String(length),
        points: Math.max(32, Math.min(1024, Math.ceil(width))), maxDepth: 255,
    }, indexedEvents);
}

export type SeekBeatWindow = { first: bigint; length: bigint };

export function seekBeatWindow(visible: SeekBeatWindow, sample: bigint, forward: boolean): SeekBeatWindow | undefined {
    const { first, length } = visible;
    if (length <= 0n || length >= (1n << 61n)) return;
    const relative = BigInt.asIntN(64, sample - first);
    const lead = length / 4n || 1n;
    const ahead = relative + (forward ? lead : -lead);
    const tile = ahead - (ahead % length + length) % length;
    if (tile === 0n) return;
    // Adjacent windows overlap by one viewport. The window changes before
    // reaching its edge and still contains recently traversed detents.
    return { first: first + tile - length / 2n, length: 2n * length };
}

export function snapTimelineSample(index: bigint, marks: readonly bigint[], tolerance: number): bigint {
    let snapped = index;
    let nearest = tolerance;
    for (const mark of marks) {
        const distance = Math.abs(Number(BigInt.asIntN(64, mark - index)));
        if (distance <= nearest) { snapped = mark; nearest = distance; }
    }
    return snapped;
}

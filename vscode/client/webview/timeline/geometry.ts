import { automationSampleIndex, automationSampleOffset } from "../../src/automationCurveGeometry";

export type RulerTick = { x: number; label: string; sampleLabel?: string; minor?: boolean };

// Shared numeric graduation geometry. The domain owns its units and labels;
// timeline and meter renderers share tick placement and visual presentation.
export function numericTicks(begin: number, end: number, width: number,
    format: (value: number) => string = String, minimumSpacing = 90): Array<RulerTick & { value: number }> {
    const duration = end - begin;
    if (!(duration > 0) || !(width > 0) || !Number.isFinite(duration) || !Number.isFinite(width)) return [];
    const targetStep = duration / Math.max(1, Math.floor(width / minimumSpacing));
    const power = 10 ** Math.floor(Math.log10(targetStep));
    const step = [1, 2, 5, 10].map((multiple) => multiple * power).find((value) => value >= targetStep)!;
    const first = Math.ceil(begin / step), last = Math.floor(end / step);
    const digits = Math.max(0, -Math.floor(Math.log10(step)));
    const ticks: Array<RulerTick & { value: number }> = [];
    const count = Math.min(1000, Math.max(0, last - first + 1));
    for (let offset = 0; offset < count; ++offset) {
        const raw = (first + offset) * step;
        if (ticks.length && ticks[ticks.length - 1].value === raw) continue;
        const value = Number(raw.toFixed(Math.min(12, digits)));
        if (ticks.length && ticks[ticks.length - 1].value === value) continue;
        ticks.push({ value, x: (raw - begin) / duration * width, label: format(value) });
    }
    return ticks;
}

// Frontend geometry only; this window makes no claim that audio data exist.
export type TimelineWindow = { beginSeconds: number; endSeconds: number };
export function timelineTicks(window: TimelineWindow, width: number) {
    return numericTicks(window.beginSeconds, window.endSeconds, width, (value) => `${value} s`)
        .map(({ value, ...tick }) => ({ ...tick, seconds: value }));
}

export function automationTimelineTicks(window: TimelineWindow & { spanSeconds?: number }, width: number, rate?: number): RulerTick[] {
    const major = timelineTicks(window, width);
    const span = window.spanSeconds ?? window.endSeconds - window.beginSeconds;
    if (!rate || !(span > 0 && width > 0)) return major;
    const ticks: RulerTick[] = major.map(({ x, label, seconds }) => {
        const index = automationSampleIndex(seconds, rate);
        return { x, label, sampleLabel: index === undefined ? undefined : `${index} smp` };
    });
    const spacing = width / (span * rate);
    const first = automationSampleIndex(window.beginSeconds, rate);
    if (first === undefined || !Number.isFinite(spacing) || spacing < 6) return ticks;
    const x = automationSampleOffset(first, window.beginSeconds, rate) / span * width;
    const count = Math.min(1000, Math.max(0, Math.floor((width - x) / spacing) + 1));
    for (let i = 0; i < count; ++i) {
        const position = x + i * spacing;
        if (position >= 0 && !major.some((tick) => Math.abs(tick.x - position) < 1))
            ticks.push({ x: position, label: "", minor: true });
    }
    return ticks.sort((a, b) => a.x - b.x);
}

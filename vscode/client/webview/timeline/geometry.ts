export type RulerTick = { x: number; label: string };

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

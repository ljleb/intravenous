// Frontend geometry only. These seconds describe the visible ruler, not
// playback position, output coverage, or a claim that data exists there.
export type TimelineWindow = { beginSeconds: number; endSeconds: number };

export function timelineTicks(window: TimelineWindow, width: number) {
    const duration = window.endSeconds - window.beginSeconds;
    if (!(duration > 0) || !(width > 0) || !Number.isFinite(duration) || !Number.isFinite(width)) return [];
    const targetStep = duration / Math.max(1, Math.floor(width / 90));
    const power = 10 ** Math.floor(Math.log10(targetStep));
    const step = [1, 2, 5, 10].map((multiple) => multiple * power).find((value) => value >= targetStep)!;
    const ticks: Array<{ seconds: number; x: number; label: string }> = [];
    const first = Math.ceil(window.beginSeconds / step);
    const last = Math.floor(window.endSeconds / step);
    const digits = Math.max(0, -Math.floor(Math.log10(step)));
    // Bound iteration independently of absolute time. Incrementing a large
    // floating-point time index can otherwise fail to advance at all.
    const count = Math.min(1000, Math.max(0, last - first + 1));
    for (let offset = 0; offset < count; ++offset) {
        const seconds = (first + offset) * step;
        if (ticks.length && ticks[ticks.length - 1].seconds === seconds) continue;
        ticks.push({ seconds, x: (seconds - window.beginSeconds) / duration * width,
            label: `${seconds.toFixed(Math.min(12, digits))} s` });
    }
    return ticks;
}


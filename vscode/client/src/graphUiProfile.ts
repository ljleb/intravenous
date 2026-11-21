export type GraphUiTiming = {
    count: number;
    totalMs: number;
    p95Ms: number;
    maxMs: number;
    sampled: number;
};

export type GraphUiProfileSummary = {
    durationMs: number;
    frameIntervals: GraphUiTiming;
    framesOver25Ms: number;
    framesOver50Ms: number;
    longAnimationFrames: GraphUiTiming | null;
    work: Record<string, GraphUiTiming>;
    lanes: number;
    expandedModules: number;
    connections: number;
    drawnWires: number;
    laneHeight: number;
};

const sampleLimit = 4096;

export class GraphUiTimingSamples {
    private readonly samples: number[] = [];
    private count = 0;
    private totalMs = 0;
    private maxMs = 0;

    add(ms: number): void {
        this.samples[this.count % sampleLimit] = ms;
        ++this.count;
        this.totalMs += ms;
        this.maxMs = Math.max(this.maxMs, ms);
    }

    summary(): GraphUiTiming {
        const sorted = this.samples.slice().sort((a, b) => a - b);
        return { count: this.count, totalMs: this.totalMs, maxMs: this.maxMs,
            p95Ms: sorted[Math.ceil(sorted.length * .95) - 1] ?? 0, sampled: sorted.length };
    }
}

export class GraphUiTimings {
    private readonly work = new Map<string, GraphUiTimingSamples>();

    record(label: string, ms: number): void {
        let samples = this.work.get(label);
        if (!samples) {
            samples = new GraphUiTimingSamples();
            this.work.set(label, samples);
        }
        samples.add(ms);
    }

    measure<T>(label: string, run: () => T): T {
        const start = performance.now();
        try { return run(); }
        finally { this.record(label, performance.now() - start); }
    }

    async measureAsync<T>(label: string, run: () => PromiseLike<T>): Promise<T> {
        const start = performance.now();
        try { return await run(); }
        finally { this.record(label, performance.now() - start); }
    }

    summary(): Record<string, GraphUiTiming> {
        return Object.fromEntries([...this.work].map(([label, samples]) => [label, samples.summary()]));
    }
}

function tail(timing: GraphUiTiming): string {
    const sampleNote = timing.sampled < timing.count ? ` (last ${timing.sampled})` : "";
    return `p95 ${timing.p95Ms.toFixed(2)} ms${sampleNote}, max ${timing.maxMs.toFixed(2)} ms`;
}

export function graphUiProfileReport(summary: GraphUiProfileSummary, host: Record<string, GraphUiTiming>): string[] {
    const lines = [`Project graph UI profile over ${(summary.durationMs / 1000).toFixed(2)} s`,
        `  At finish: ${summary.lanes} lanes at ${summary.laneHeight.toFixed(1)} px, ${summary.expandedModules} expanded modules, `
            + `${summary.connections} connections in scope, ${summary.drawnWires} wires drawn.`];
    const frames = summary.frameIntervals;
    lines.push(frames.count ? `  Frame intervals: ${frames.count}, ${tail(frames)}, `
        + `${summary.framesOver25Ms} over 25 ms, ${summary.framesOver50Ms} over 50 ms.`
        : "  No frame intervals recorded. Keep the graph editor visible during the recording.");
    const longFrames = summary.longAnimationFrames;
    lines.push(longFrames ? `  Browser long animation frames (50 ms or more): ${longFrames.count}`
        + (longFrames.count ? `, ${tail(longFrames)}.` : ".")
        : "  Browser long animation frame reporting is unavailable.");
    const appendTimings = (title: string, entries: Record<string, GraphUiTiming>) => {
        lines.push(`  ${title}`);
        for (const [label, timing] of Object.entries(entries).sort(([, a], [, b]) => b.totalMs - a.totalMs)) {
            const calls = timing.count === 1 ? "1 call" : `${timing.count} calls`;
            lines.push(`    ${label}: ${calls}, ${timing.totalMs.toFixed(2)} ms total, ${tail(timing)}.`);
        }
        if (!Object.keys(entries).length) lines.push("    No calls recorded.");
    };
    appendTimings("Webview timings (some overlap)", summary.work);
    appendTimings("Extension host waits (can overlap)", host);
    return lines;
}

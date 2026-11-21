import { GraphUiTimingSamples, GraphUiTimings, type GraphUiProfileSummary } from "../../src/graphUiProfile";

type GraphCounts = Pick<GraphUiProfileSummary, "lanes" | "expandedModules" | "connections" | "drawnWires" | "laneHeight">;

export class GraphUiRecording {
    readonly work = new GraphUiTimings();
    private readonly startedAt = performance.now();
    private readonly frameIntervals = new GraphUiTimingSamples();
    private readonly longAnimationFrames = new GraphUiTimingSamples();
    private readonly observer?: PerformanceObserver;
    private previousFrame: number | undefined;
    private frame: number;
    private framesOver25Ms = 0;
    private framesOver50Ms = 0;
    private visible = true;

    constructor() {
        if (globalThis.PerformanceObserver?.supportedEntryTypes?.includes("long-animation-frame")) {
            this.observer = new PerformanceObserver((list) => this.recordLongFrames(list.getEntries()));
            this.observer.observe({ type: "long-animation-frame" });
        }
        document.addEventListener("visibilitychange", this.resetFrameInterval);
        this.frame = requestAnimationFrame(this.recordFrame);
    }

    setVisible(visible: boolean): void {
        if (this.visible === visible) return;
        this.visible = visible;
        this.resetFrameInterval();
    }

    private readonly resetFrameInterval = () => { this.previousFrame = undefined; };

    private readonly recordFrame = (time: number) => {
        if (this.visible && !document.hidden) {
            if (this.previousFrame !== undefined) {
                const interval = time - this.previousFrame;
                this.frameIntervals.add(interval);
                if (interval > 25) ++this.framesOver25Ms;
                if (interval > 50) ++this.framesOver50Ms;
            }
            this.previousFrame = time;
        }
        this.frame = requestAnimationFrame(this.recordFrame);
    };

    private recordLongFrames(entries: PerformanceEntry[]): void {
        for (const entry of entries) {
            if (entry.startTime >= this.startedAt) this.longAnimationFrames.add(entry.duration);
        }
    }

    finish(counts: GraphCounts): GraphUiProfileSummary {
        const durationMs = performance.now() - this.startedAt;
        if (this.observer) this.recordLongFrames(this.observer.takeRecords());
        this.dispose();
        return { ...counts, durationMs, frameIntervals: this.frameIntervals.summary(),
            framesOver25Ms: this.framesOver25Ms, framesOver50Ms: this.framesOver50Ms,
            longAnimationFrames: this.observer ? this.longAnimationFrames.summary() : null, work: this.work.summary() };
    }

    dispose(): void {
        this.observer?.disconnect();
        document.removeEventListener("visibilitychange", this.resetFrameInterval);
        cancelAnimationFrame(this.frame);
    }
}

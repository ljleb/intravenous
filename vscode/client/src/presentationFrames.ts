type FrameWork = (time: number) => void;
type FramePhase = "measure" | "update";

// Work requested by a measurement can update the same frame. Work requested
// during the update phase waits until the next frame, including the next read.
export class PresentationFrames {
    private readonly queues = { measure: new Set<FrameWork>(), update: new Set<FrameWork>() };
    private running: Set<FrameWork> | undefined;
    private frame: number | undefined;
    private flushing = false;

    constructor(private readonly request: (run: FrameWork) => number,
        private readonly cancelFrame: (id: number) => void,
        private readonly report: (error: unknown) => void) {}

    schedule(phase: FramePhase, run: FrameWork): void {
        if (this.running?.has(run)) return;
        this.queues[phase].add(run);
        this.ensureFrame();
    }

    cancel(run: FrameWork): void {
        this.queues.measure.delete(run);
        this.queues.update.delete(run);
        this.running?.delete(run);
        if (this.frame !== undefined && !this.queues.measure.size && !this.queues.update.size) {
            this.cancelFrame(this.frame);
            this.frame = undefined;
        }
    }

    private ensureFrame(): void {
        if (!this.flushing && this.frame === undefined && (this.queues.measure.size || this.queues.update.size))
            this.frame = this.request(this.flush);
    }

    private readonly flush = (time: number): void => {
        this.frame = undefined;
        this.flushing = true;
        for (const phase of ["measure", "update"] as const) {
            this.running = this.queues[phase];
            this.queues[phase] = new Set();
            for (const run of this.running) {
                this.running.delete(run);
                try { run(time); }
                catch (error) { this.report(error); }
            }
        }
        this.running = undefined;
        this.flushing = false;
        this.ensureFrame();
    };
}

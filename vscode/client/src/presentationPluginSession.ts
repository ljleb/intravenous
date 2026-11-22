import { readPresentationFrame, type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "./presentationPlugin";

type Run = (snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]) => PresentationFrame | Promise<unknown>;

export class PresentationWake {
    private timer?: ReturnType<typeof setTimeout>;
    private deadline = Infinity;
    constructor(private readonly wake: () => void) {}
    get pending(): boolean { return this.deadline !== Infinity; }
    request(afterMs: number): void {
        const delay = Math.max(8, afterMs);
        const deadline = performance.now() + delay;
        if (deadline >= this.deadline) return;
        this.cancel();
        this.deadline = deadline;
        this.timer = setTimeout(() => { this.cancel(); this.wake(); }, delay);
    }
    cancel(): void {
        clearTimeout(this.timer);
        this.timer = undefined;
        this.deadline = Infinity;
    }
}

/** Coalesces redraws while delivering gestures in their original order. */
export class PresentationPluginSession {
    private snapshot: PresentationSnapshot | undefined;
    private viewport = "";
    private inputs: PresentationInput[] = [];
    private drawing: PresentationFrame["marks"];
    private dirty = false;
    private running = false;
    private closed = false;

    constructor(private readonly run: Run, private readonly schedule: () => void,
        private readonly accept: (frame: PresentationFrame, draw: boolean, inputs: readonly PresentationInput[]) => void,
        private readonly fail: (error: unknown) => void) {}

    get pending(): boolean { return !this.closed && (this.dirty || this.running || this.inputs.length > 0); }

    update(snapshot: PresentationSnapshot, pacing: "frame" | "immediate" = "frame"): void {
        this.snapshot = snapshot;
        this.viewport = JSON.stringify(snapshot.viewport);
        this.dirty = true;
        if (pacing === "immediate") void this.flush();
        else this.request();
    }

    input(input: PresentationInput, pacing: "frame" | "immediate" = "frame"): void {
        if (this.closed) return;
        if (this.inputs.length >= 512) {
            this.stop(new Error("Presentation input queue exceeded its limit"));
            return;
        }
        this.inputs.push(input);
        if (pacing === "immediate") void this.flush();
        else this.request();
    }

    suspend(): void { this.dirty = false; }

    private request(): void {
        if (!this.closed && !this.running && this.snapshot && (this.dirty || this.inputs.length)) this.schedule();
    }

    async flush(): Promise<void> {
        if (this.closed || this.running || !this.snapshot || (!this.dirty && !this.inputs.length)) return;
        this.running = true;
        this.dirty = false;
        const inputs = this.inputs;
        this.inputs = [];
        const viewport = this.viewport;
        const snapshot = this.snapshot;
        try {
            const reply = this.run({ ...snapshot, clockMs: performance.now() }, inputs);
            const result = reply instanceof Promise ? await reply : reply;
            if (!this.closed) {
                const frame = readPresentationFrame(result);
                if (frame.reuseDrawing) {
                    if (!this.drawing) throw new Error("Presentation has no drawing to reuse");
                    delete frame.reuseDrawing;
                    frame.marks = this.drawing;
                } else this.drawing = frame.marks;
                this.accept(frame, viewport === this.viewport, inputs);
            }
        } catch (error) { this.stop(error); }
        finally {
            this.running = false;
            this.request();
        }
    }

    private stop(error: unknown): void {
        if (this.closed) return;
        this.dispose();
        this.fail(error);
    }

    dispose(): void {
        this.closed = true;
        this.inputs = [];
        this.snapshot = undefined;
        this.drawing = undefined;
    }
}

import { interruptedPresentationEditReplies, readPresentationFrame, samePresentationLayout,
    type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "./presentationPlugin";
import type { SerializedPresentationState } from "./presentationStateJson";

type Run = (snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]) => PresentationFrame | Promise<unknown>;
type SaveHandling = {
    canSave(inputs: readonly PresentationInput[]): boolean;
    save(state: SerializedPresentationState, inputs: readonly PresentationInput[]): void;
    restore(state: SerializedPresentationState): void;
};

export class PresentationWake {
    private timer?: ReturnType<typeof setTimeout>;
    private deadline = Infinity;
    constructor(private readonly wake: () => void) {}
    get pending(): boolean { return this.deadline !== Infinity; }
    follow(frame: Pick<PresentationFrame, "wakeAfterMs">): void {
        if (frame.wakeAfterMs !== undefined) this.request(frame.wakeAfterMs);
    }
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
    private timelineGrid: PresentationFrame["timelineGrid"];
    private dirty = false;
    private running = false;
    private closed = false;
    private interruption = 0;
    private idleWaiters: Array<() => void> = [];

    constructor(private readonly run: Run, private readonly schedule: () => void,
        private readonly accept: (frame: PresentationFrame, draw: boolean, inputs: readonly PresentationInput[]) => void,
        private readonly fail: (error: unknown) => void, private readonly saves?: SaveHandling) {}

    get pending(): boolean { return !this.closed && (this.dirty || this.running || this.inputs.length > 0); }

    whenIdle(): Promise<void> {
        return this.pending ? new Promise((resolve) => { this.idleWaiters.push(resolve); }) : Promise.resolve();
    }

    update(snapshot: PresentationSnapshot, pacing: "frame" | "immediate" = "frame"): void {
        this.setSnapshot(snapshot);
        this.dirty = true;
        if (pacing === "immediate") void this.flush();
        else this.request();
    }

    updateViewport(viewport: PresentationSnapshot["viewport"], observations: PresentationSnapshot["observations"]): void {
        if (this.snapshot) this.setSnapshot({ ...this.snapshot, viewport, observations });
    }

    private setSnapshot(snapshot: PresentationSnapshot): void {
        this.snapshot = snapshot;
        this.viewport = JSON.stringify(snapshot.viewport);
    }

    input(input: PresentationInput, pacing: "frame" | "immediate" = "frame"): void {
        if (this.closed) return;
        if (this.inputs.length >= 512) {
            this.stop(new Error("Presentation input queue exceeded its limit"));
            return;
        }
        this.inputs.push(input.type === "gesture" && !input.viewport && this.snapshot?.viewport.time
            ? { ...input, viewport: this.snapshot.viewport } : input);
        if (pacing === "immediate") void this.flush();
        else this.request();
    }

    interrupt(reason: "history" | "disconnected", replies: PresentationInput[] = [],
        pacing: "frame" | "immediate" = "frame"): void {
        if (this.closed) return;
        ++this.interruption;
        this.inputs = [...this.inputs.filter(({ type }) => type === "editResult"), ...replies];
        this.input({ type: "interrupt", reason }, pacing);
    }

    suspend(): void {
        this.dirty = false;
        this.resolveIdle();
    }

    private resolveIdle(): void {
        if (this.pending) return;
        for (const resolve of this.idleWaiters.splice(0)) resolve();
    }

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
        const interruption = this.interruption;
        const snapshot = this.snapshot;
        try {
            const reply = this.run({ ...snapshot, clockMs: performance.now() }, inputs);
            const result = reply instanceof Promise ? await reply : reply;
            if (!this.closed) {
                const frame = readPresentationFrame(result);
                const latest = this.snapshot?.viewport;
                const sameLayout = frame.timelineCurve && !frame.timelineCurve.viewportDrawing && latest
                    && samePresentationLayout(snapshot.viewport, latest);
                this.acceptFrame(frame, inputs, interruption === this.interruption, viewport === this.viewport || !!sameLayout,
                    snapshot.state === this.snapshot?.state);
            }
        } catch (error) { this.stop(error); }
        finally {
            this.running = false;
            this.resolveIdle();
            this.request();
        }
    }

    private acceptFrame(frame: PresentationFrame, inputs: readonly PresentationInput[], current: boolean,
        sameViewport: boolean, sameState: boolean): void {
        if (frame.reuseDrawing) {
            if (!this.drawing) throw new Error("Presentation has no drawing to reuse");
            delete frame.reuseDrawing;
            frame.marks = this.drawing;
            frame.timelineGrid = this.timelineGrid;
        }

        const { saveRequest } = frame;
        const discardedSave = saveRequest !== undefined && (!current || !sameState || !this.saves?.canSave(inputs));
        const draw = sameState && !discardedSave;
        if (!draw) {
            if (discardedSave) this.saves?.restore(this.snapshot?.state ?? null);
            delete frame.timelineCurve;
        }
        else {
            this.drawing = frame.marks;
            this.timelineGrid = frame.timelineGrid;
        }
        if (saveRequest !== undefined) delete frame.saveRequest;

        if (!current) {
            for (const reply of interruptedPresentationEditReplies(frame.commands ?? [])) this.input(reply);
            return;
        }
        this.accept(frame, sameViewport && draw, inputs);
        if (saveRequest !== undefined && !discardedSave && this.snapshot) {
            this.snapshot = { ...this.snapshot, state: saveRequest };
            this.saves?.save(saveRequest, inputs);
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
        this.timelineGrid = undefined;
        this.resolveIdle();
    }
}

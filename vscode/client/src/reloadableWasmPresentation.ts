import { readPresentationFrame, type PresentationInput, type PresentationSavedState, type PresentationSnapshot } from "./presentationPlugin";
import type { WasmPresentationWorker } from "./wasmPresentationWorker";

/** A candidate proves it can restore and draw before it takes over a live lane. */
export class ReloadableWasmPresentation {
    private active?: { bytes: string; worker: WasmPresentationWorker };
    private candidate?: WasmPresentationWorker;
    private rejected?: string;
    private revision = 0;
    private state: PresentationSavedState = null;

    constructor(private readonly start: (bytes: string) => WasmPresentationWorker,
        private readonly report: (error?: unknown) => void) {}

    restore(state: PresentationSavedState): void {
        ++this.revision;
        this.state = state;
        this.rejected = undefined;
        this.active?.worker.restore(state);
    }

    hasReplacement(bytes: string): boolean {
        return !!this.active && bytes !== this.active.bytes && bytes !== this.rejected;
    }

    async run(bytes: string, snapshot: PresentationSnapshot, inputs: readonly PresentationInput[], canReload: () => boolean): Promise<unknown> {
        if (!this.active) {
            this.active = { bytes, worker: this.start(bytes) };
            this.active.worker.restore(this.state);
        }
        const previous = this.active;
        if (this.hasReplacement(bytes) && !inputs.length && canReload()) {
            const candidate = this.start(bytes);
            const revision = this.revision;
            candidate.restore(this.state);
            this.candidate = candidate;
            try {
                const frame = readPresentationFrame(await candidate.run(snapshot, []));
                if (frame.reuseDrawing) throw new Error("Replacement presentation has no drawing to reuse");
                if (this.active === previous && this.revision === revision && canReload()) {
                    this.active = { bytes, worker: candidate };
                    previous.worker.stop();
                    this.report();
                    return frame;
                }
            } catch (error) {
                if (this.active === previous && this.revision === revision && canReload()) {
                    this.rejected = bytes;
                    this.report(error);
                }
            } finally {
                this.candidate = undefined;
                if (this.active?.worker !== candidate) candidate.stop();
            }
        }
        if (this.active !== previous) throw new Error("Presentation session closed");
        const reply = await previous.worker.run(snapshot, inputs);
        // A committed gesture can update the live session before its save
        // reaches the project store. The snapshot for a later reload uses it.
        const saved = reply?.commands?.find((command) => command.type === "state");
        if (saved?.type === "state") this.state = saved.value;
        return reply;
    }

    suspend(): void {
        this.active?.worker.stop();
        this.candidate?.stop();
    }

    dispose(): void {
        this.suspend();
        this.active = undefined;
    }
}

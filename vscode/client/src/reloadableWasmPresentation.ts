import { readPresentationFrame, type PresentationFrame, type PresentationInput, type PresentationSnapshot } from "./presentationPlugin";
import type { SerializedPresentationState } from "./presentationStateJson";
import type { WasmPresentationWorker } from "./wasmPresentationWorker";

/** A candidate proves it can restore and draw before it takes over a live lane. */
export class ReloadableWasmPresentation {
    private active?: { bytes: string; worker: WasmPresentationWorker };
    private candidate?: WasmPresentationWorker;
    private rejected?: string;
    private revision = 0;

    constructor(private readonly start: (bytes: string) => WasmPresentationWorker,
        private readonly report: (error?: unknown) => void) {}

    restore(state: SerializedPresentationState): void {
        ++this.revision;
        this.rejected = undefined;
        this.active?.worker.restore(state);
    }

    hasReplacement(bytes: string): boolean {
        return !!this.active && bytes !== this.active.bytes && bytes !== this.rejected;
    }

    async run(bytes: string, snapshot: PresentationSnapshot, inputs: readonly PresentationInput[],
        canReload: () => boolean): Promise<PresentationFrame | undefined> {
        this.active ??= { bytes, worker: this.start(bytes) };
        const previous = this.active;
        if (this.hasReplacement(bytes) && !inputs.length && canReload()) {
            const candidate = this.start(bytes);
            const revision = this.revision;
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
        return previous.worker.run(snapshot, inputs);
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

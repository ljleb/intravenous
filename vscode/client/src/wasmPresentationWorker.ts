import type { PresentationFrame, PresentationInput, PresentationSnapshot } from "./presentationPlugin";
import { wasmPresentationUpdate, type WasmPresentationReply } from "./wasmPresentation";
import type { SerializedPresentationState } from "./presentationStateJson";

import type { WasmPresentationRequest } from "./wasmPresentationEndpoint";
import type { WasmConstruction } from "./wasmPresentationArguments";
type WorkerTransport = { postMessage(request: WasmPresentationRequest): void; terminate(): void };
type StartWorker = (receive: (reply: WasmPresentationReply) => void, fail: (error: Error) => void) => WorkerTransport;
let nextSession = 0;

export class WasmPresentationWorker {
    private worker?: WorkerTransport;
    private session = 0;
    private pendingRestore?: SerializedPresentationState;
    private construction?: WasmConstruction | null;
    private curvePoints?: ArrayBuffer;
    private hostWaveforms: string[] = [];
    private pending?: { resolve: (value: PresentationFrame | undefined) => void; reject: (error: Error) => void; timeout: ReturnType<typeof setTimeout> };
    private readonly closing = new Map<WorkerTransport, () => void>();

    constructor(private readonly bytes: string, private readonly start: StartWorker) {}

    restore(state: SerializedPresentationState): void {
        this.pendingRestore = state;
    }

    run(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]): Promise<PresentationFrame | undefined> {
        if (this.pending) return Promise.reject(new Error("A presentation frame is already running"));
        const initialize = !this.worker;
        if (!this.worker) {
            this.session = ++nextSession;
            const worker = this.start((reply) => {
                if (this.closing.has(worker)) {
                    if (reply.closed || reply.error) this.closing.get(worker)?.();
                    return;
                }
                if (this.worker !== worker || !this.pending) return;
                if (typeof reply?.error === "string") {
                    this.stop(new Error(reply.error));
                    return;
                }
                if (reply?.frame?.timelineCurve) {
                    if (reply.reuseCurvePoints) {
                        if (!this.curvePoints) {
                            this.stop(new Error("Presentation has no retained curve points"));
                            return;
                        }
                        reply.frame.timelineCurve.points = this.curvePoints;
                    } else this.curvePoints = reply.frame.timelineCurve.points;
                    const { minimumInput, maximumInput } = reply.frame.timelineCurve;
                    this.hostWaveforms = [minimumInput, maximumInput];
                } else {
                    this.hostWaveforms = [];
                }
                const { resolve, timeout } = this.pending;
                this.pending = undefined;
                clearTimeout(timeout);
                // An external state change takes precedence over this frame's save.
                if (reply?.frame && this.pendingRestore !== undefined) delete reply.frame.saveRequest;
                resolve(reply?.frame);
            }, (error) => {
                if (this.worker === worker) this.stop(error);
                else this.closing.get(worker)?.();
            });
            this.worker = worker;
        }
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => this.stop(new Error("Presentation worker timed out")), 1500);
            this.pending = { resolve, reject, timeout };
            try {
                const message: WasmPresentationRequest = { update: wasmPresentationUpdate(snapshot, inputs, this.session, this.hostWaveforms) };
                const construction = message.update.construction;
                if (!initialize && this.construction?.definitionId === construction?.definitionId
                    && this.construction?.arguments === construction?.arguments) delete message.update.construction;
                else this.construction = construction;
                if (initialize) message.bytes = this.bytes;
                if (initialize || this.pendingRestore !== undefined) {
                    const state = this.pendingRestore === undefined ? snapshot.state : this.pendingRestore;
                    message.restore = state ?? "";
                    this.pendingRestore = undefined;
                }
                this.worker!.postMessage(message);
            }
            catch (error) { this.stop(error instanceof Error ? error : new Error(String(error))); }
        });
    }

    stop(error?: Error): void {
        const pending = this.pending;
        this.pending = undefined;
        const worker = this.worker;
        this.worker = undefined;
        this.curvePoints = undefined;
        this.hostWaveforms = [];
        if (pending) {
            clearTimeout(pending.timeout);
            pending.reject(error ?? new Error("Presentation worker closed"));
        }
        if (!worker) return;
        if (error) {
            worker.terminate();
            return;
        }
        const finish = () => {
            if (!this.closing.delete(worker)) return;
            clearTimeout(timeout);
            worker.terminate();
        };
        const timeout = setTimeout(finish, 250);
        this.closing.set(worker, finish);
        try { worker.postMessage({ close: true }); }
        catch { finish(); }
    }
}

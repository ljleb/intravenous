import type { PresentationFrame, PresentationInput, PresentationSavedState, PresentationSnapshot } from "./presentationPlugin";
import { wasmPresentationUpdate, type WasmPresentationReply } from "./wasmPresentation";

import type { WasmPresentationRequest } from "./wasmPresentationEndpoint";
type WorkerTransport = { postMessage(request: WasmPresentationRequest): void; terminate(): void };
type StartWorker = (receive: (reply: WasmPresentationReply) => void, fail: (error: Error) => void) => WorkerTransport;

export class WasmPresentationWorker {
    private worker?: WorkerTransport;
    private state: PresentationSavedState = null;
    private restoring = true;
    private pending?: { resolve: (value: PresentationFrame | undefined) => void; reject: (error: Error) => void; timeout: ReturnType<typeof setTimeout> };
    private readonly closing = new Map<WorkerTransport, () => void>();

    constructor(private readonly bytes: string, private readonly start: StartWorker) {}

    restore(state: PresentationSavedState): void {
        this.state = state;
        this.restoring = true;
    }

    run(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]): Promise<PresentationFrame | undefined> {
        if (this.pending) return Promise.reject(new Error("A presentation frame is already running"));
        const initialize = !this.worker;
        if (!this.worker) {
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
                const { resolve, timeout } = this.pending;
                this.pending = undefined;
                clearTimeout(timeout);
                if (reply?.frame && reply.savedState !== undefined && !this.restoring) {
                    this.state = reply.savedState;
                    reply.frame.commands = [{ type: "state", value: reply.savedState }];
                }
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
                const message: WasmPresentationRequest = { update: wasmPresentationUpdate(snapshot, inputs) };
                if (initialize) message.bytes = this.bytes;
                if (initialize || this.restoring) message.restore = this.state === null ? "" : JSON.stringify(this.state);
                this.restoring = false;
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

import type { PresentationFrame, PresentationInput, PresentationSnapshot } from "./presentationPlugin";
import { wasmPresentationUpdate, type WasmPresentationReply } from "./wasmPresentation";
import type { SerializedPresentationState } from "./presentationStateJson";

import type { WasmPresentationProgress, WasmPresentationRequest, WasmPresentationWorkerMessage } from "./wasmPresentationEndpoint";
import type { WasmConstruction } from "./wasmPresentationArguments";
type WorkerTransport = { postMessage(request: WasmPresentationRequest): void; terminate(): void };
type StartWorker = (receive: (reply: WasmPresentationWorkerMessage) => void, fail: (error: Error) => void) => WorkerTransport;
let nextSession = 0;
const timeoutMs = 1500;

type FrameStage = WasmPresentationProgress["stage"] | "start" | "prepare" | "posted";
class WorkerFrame {
    readonly startedAt = performance.now();
    stage: FrameStage = "start";
    stageAt = this.startedAt;
    message?: Extract<WasmPresentationRequest, { update: unknown }>;

    constructor(readonly session: number, readonly frame: number, private readonly snapshot: PresentationSnapshot,
        private readonly inputs: readonly PresentationInput[]) {}

    advance(stage: FrameStage) {
        this.stage = stage;
        this.stageAt = performance.now();
    }

    failure(error: Error): PresentationWorkerError {
        const now = performance.now();
        const { snapshot, inputs, message } = this;
        const inputCounts: Record<string, number> = {};
        for (const input of inputs) {
            const key = "phase" in input ? `${input.type}.${input.phase}` : input.type;
            inputCounts[key] = (inputCounts[key] ?? 0) + 1;
        }
        return new PresentationWorkerError(error, {
            session: this.session, frame: this.frame, stage: this.stage, timeoutMs,
            elapsedMs: Math.round(now - this.startedAt), stageElapsedMs: Math.round(now - this.stageAt),
            revision: snapshot.revision, mode: snapshot.mode, editable: snapshot.editable,
            editsPending: !!snapshot.editsPending, viewport: snapshot.viewport, inputs: inputCounts,
            constructionArguments: snapshot.node.arguments.length,
            argumentArrayLengths: snapshot.node.arguments.slice(0, 16).map((argument) => Array.isArray(argument) ? argument.length : null),
            ...(message && { initializing: message.bytes !== undefined, sendingConstruction: "construction" in message.update,
                restoreBytes: message.restore === undefined ? undefined : new TextEncoder().encode(message.restore).byteLength,
                triggerColumns: message.update.triggerColumns.length, waveformCount: message.update.waveforms.length,
                waveforms: message.update.waveforms.slice(0, 16).map(({ id, status, regions }) => ({ id, status, regions: regions.length,
                    bytes: regions.reduce((sum, region) => sum + region.points.byteLength, 0) })) }),
        });
    }
}

export class PresentationWorkerError extends Error {
    constructor(error: Error, readonly frame: Record<string, unknown>) {
        super(error.message, { cause: error });
        this.stack = error.stack;
    }
}

export class WasmPresentationWorker {
    private worker?: WorkerTransport;
    private session = 0;
    private frameNumber = 0;
    private pendingRestore?: SerializedPresentationState;
    private construction?: WasmConstruction | null;
    private curvePoints?: ArrayBuffer;
    private hostWaveforms: string[] = [];
    private pending?: { frame: WorkerFrame; resolve: (value: PresentationFrame | undefined) => void;
        reject: (error: Error) => void; timeout: ReturnType<typeof setTimeout> };
    private readonly closing = new Map<WorkerTransport, () => void>();

    constructor(private readonly bytes: string, private readonly start: StartWorker) {}

    restore(state: SerializedPresentationState): void {
        this.pendingRestore = state;
    }

    run(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]): Promise<PresentationFrame | undefined> {
        if (this.pending) return Promise.reject(new Error("A presentation frame is already running"));
        const initialize = !this.worker;
        if (initialize) { this.session = ++nextSession; this.frameNumber = 0; }
        const frame = new WorkerFrame(this.session, ++this.frameNumber, snapshot, inputs);
        if (!this.worker) {
            try {
                this.worker = this.startWorker();
            } catch (error) {
                return Promise.reject(frame.failure(error instanceof Error ? error : new Error(String(error))));
            }
        }
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => this.stop(new Error("Presentation worker timed out")), timeoutMs);
            this.pending = { frame, resolve, reject, timeout };
            try {
                frame.advance("prepare");
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
                frame.message = message;
                this.worker!.postMessage(message);
                if (frame.stage === "prepare") frame.advance("posted");
            }
            catch (error) { this.stop(error instanceof Error ? error : new Error(String(error))); }
        });
    }

    private startWorker() {
        const worker = this.start((message) => {
            if (!message) return;
            if ("stage" in message) {
                if (this.worker === worker) this.pending?.frame.advance(message.stage);
                return;
            }
            const reply: WasmPresentationReply = message;
            if (this.closing.has(worker)) {
                if (reply.closed || reply.error) this.closing.get(worker)?.();
                return;
            }
            if (this.worker !== worker || !this.pending) return;
            if (typeof reply?.error === "string") {
                const error = new Error(reply.error);
                if (reply.errorStack) error.stack = reply.errorStack;
                this.stop(error);
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
        return worker;
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
            pending.reject(error ? pending.frame.failure(error) : new Error("Presentation worker closed"));
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

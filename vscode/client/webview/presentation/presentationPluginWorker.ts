import type { PresentationInput, PresentationSnapshot } from "../../src/presentationPlugin";

// The project artifact is a single script defining present(snapshot, inputs).
// The worker receives snapshots only for its lane. The main thread validates
// its replies before it creates controls or routes any commands.
export class PresentationPluginWorker {
    private worker?: Worker;
    private pending?: { resolve: (value: unknown) => void; reject: (error: Error) => void; timeout: ReturnType<typeof setTimeout> };

    constructor(private readonly source: string) {}

    run(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]): Promise<unknown> {
        if (this.pending) return Promise.reject(new Error("A presentation frame is already running"));
        this.worker ??= this.start();
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => this.stop(new Error("Presentation worker timed out")), 1500);
            this.pending = { resolve, reject, timeout };
            try { this.worker!.postMessage({ snapshot, inputs }); }
            catch (error) { this.stop(error instanceof Error ? error : new Error(String(error))); }
        });
    }

    private start(): Worker {
        const script = `${this.source}\n;globalThis.onmessage = ({ data }) => {\n`
            + `try { postMessage({ frame: present(data.snapshot, data.inputs) }); }\n`
            + `catch (error) { postMessage({ error: String(error).slice(0, 512) }); }\n};`;
        const url = URL.createObjectURL(new Blob([script], { type: "text/javascript" }));
        let worker: Worker;
        try { worker = new Worker(url); }
        finally { URL.revokeObjectURL(url); }
        worker.onmessage = ({ data }) => {
            const pending = this.pending;
            if (!pending || worker !== this.worker) return;
            this.pending = undefined;
            clearTimeout(pending.timeout);
            if (typeof data?.error === "string") pending.reject(new Error(data.error.slice(0, 512)));
            else pending.resolve(data?.frame);
        };
        worker.onerror = (event) => this.stop(new Error(event.message || "Presentation worker failed"));
        worker.onmessageerror = () => this.stop(new Error("Invalid presentation worker message"));
        return worker;
    }

    stop(error = new Error("Presentation worker closed")): void {
        const pending = this.pending;
        this.pending = undefined;
        this.worker?.terminate();
        this.worker = undefined;
        if (pending) { clearTimeout(pending.timeout); pending.reject(error); }
    }
}

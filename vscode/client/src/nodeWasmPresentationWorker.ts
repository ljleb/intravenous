import { Worker } from "node:worker_threads";
import { join } from "node:path";
import { WasmPresentationWorker } from "./wasmPresentationWorker";

export function createNodePresentationWorker(bytes: string): WasmPresentationWorker {
    return new WasmPresentationWorker(bytes, (receive, fail) => {
        const worker = new Worker(join(__dirname, "wasmPresentationNodeEntry.js"));
        worker.on("message", receive);
        worker.on("error", fail);
        worker.on("exit", () => fail(new Error("Presentation worker exited")));
        return worker;
    });
}

import { Worker } from "node:worker_threads";
import { wasmPresentationWorkerSource } from "./wasmPresentation";
import { WasmPresentationWorker } from "./wasmPresentationWorker";

export function createNodePresentationWorker(bytes: string): WasmPresentationWorker {
    return new WasmPresentationWorker(bytes, (receive, fail) => {
        const script = `${wasmPresentationWorkerSource()}
import("node:worker_threads").then(({ parentPort }) => {
    parentPort.on("message", (message) => {
        const reply = run(message);
        parentPort.postMessage(reply, reply.storage ? [reply.storage] : []);
    });
});
`;
        const worker = new Worker(script, { eval: true });
        worker.on("message", receive);
        worker.on("error", fail);
        worker.on("exit", () => fail(new Error("Presentation worker exited")));
        return worker;
    });
}

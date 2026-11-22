import { wasmPresentationWorkerSource } from "../../src/wasmPresentation";
import { WasmPresentationWorker } from "../../src/wasmPresentationWorker";

export function createWebPresentationWorker(bytes: string): WasmPresentationWorker {
    return new WasmPresentationWorker(bytes, (receive, fail) => {
        const script = `${wasmPresentationWorkerSource()}
onmessage = ({ data }) => {
    const reply = run(data);
    postMessage(reply, reply.storage ? [reply.storage] : []);
};`;
        const url = URL.createObjectURL(new Blob([script], { type: "text/javascript" }));
        let worker: Worker;
        try { worker = new Worker(url); }
        finally { URL.revokeObjectURL(url); }
        worker.onmessage = ({ data }) => receive(data);
        worker.onerror = (event) => fail(new Error(event.message || "Presentation worker failed"));
        worker.onmessageerror = () => fail(new Error("Invalid presentation worker message"));
        return worker;
    });
}

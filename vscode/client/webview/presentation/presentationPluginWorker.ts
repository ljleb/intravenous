import PresentationWorker from "./wasmPresentationWebEntry?worker&inline";
import { WasmPresentationWorker } from "../../src/wasmPresentationWorker";

export function createWebPresentationWorker(bytes: string): WasmPresentationWorker {
    return new WasmPresentationWorker(bytes, (receive, fail) => {
        const worker = new PresentationWorker();
        worker.onmessage = ({ data }) => receive(data);
        worker.onerror = (event) => fail(new Error(event.message || "Presentation worker failed"));
        worker.onmessageerror = () => fail(new Error("Invalid presentation worker message"));
        return worker;
    });
}

import { parentPort } from "node:worker_threads";
import { createWasmPresentationEndpoint, type WasmPresentationRequest } from "./wasmPresentationEndpoint";

if (!parentPort) throw new Error("Missing presentation worker port");
const port = parentPort;
const receive = createWasmPresentationEndpoint();
port.on("message", (request: WasmPresentationRequest) => {
    const reply = receive(request);
    port.postMessage(reply, reply.storage ? [reply.storage] : []);
});

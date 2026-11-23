import { createWasmPresentationEndpoint, wasmPresentationReplyTransfers, type WasmPresentationRequest } from "../../src/wasmPresentationEndpoint";

const receive = createWasmPresentationEndpoint();
self.onmessage = ({ data }: MessageEvent<WasmPresentationRequest>) => {
    const reply = receive(data);
    self.postMessage(reply, { transfer: wasmPresentationReplyTransfers(reply) });
};

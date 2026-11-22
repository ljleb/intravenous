import { createWasmPresentation, type WasmPresentationReply, type WasmPresentationUpdate } from "./wasmPresentation";

export type WasmPresentationRequest = { update: WasmPresentationUpdate; bytes?: string; restore?: string } | { close: true };

export function createWasmPresentationEndpoint() {
    let instance: ReturnType<typeof createWasmPresentation> | undefined;
    return (message: WasmPresentationRequest): WasmPresentationReply => {
        try {
            if ("close" in message) {
                instance?.dispose();
                instance = undefined;
                return { closed: true };
            }
            if (message.bytes !== undefined) instance = createWasmPresentation(message.bytes);
            if (!instance) throw new Error("Presentation session has not started");
            if (message.restore !== undefined) instance.restore(message.restore);
            return instance.present(message.update);
        } catch (error) {
            instance = undefined;
            return { error: String(error).slice(0, 512) };
        }
    };
}

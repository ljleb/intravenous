import { createWasmPresentation, type WasmPresentationReply, type WasmPresentationUpdate } from "./wasmPresentation";

export type WasmPresentationRequest = { update: WasmPresentationUpdate; bytes?: string; restore?: string } | { close: true };

export function wasmPresentationReplyTransfers(reply: WasmPresentationReply): ArrayBuffer[] {
    const transfers = reply.storage ? [reply.storage] : [];
    if (reply.frame?.timelineCurve && !reply.reuseCurvePoints) transfers.push(reply.frame.timelineCurve.points);
    return transfers;
}

export function createWasmPresentationEndpoint() {
    let instance: ReturnType<typeof createWasmPresentation> | undefined;
    let curvePoints: ArrayBuffer | undefined;
    return (message: WasmPresentationRequest): WasmPresentationReply => {
        try {
            if ("close" in message) {
                instance?.dispose();
                instance = undefined;
                return { closed: true };
            }
            if (message.bytes !== undefined) {
                instance = createWasmPresentation(message.bytes);
                curvePoints = undefined;
            }
            if (!instance) throw new Error("Presentation session has not started");
            if (message.restore !== undefined) instance.restore(message.restore);
            const reply = instance.present(message.update);
            const curve = reply.frame?.timelineCurve;
            if (curve && curve.points === curvePoints) {
                reply.reuseCurvePoints = true;
                curve.points = new ArrayBuffer(0);
            } else if (curve) curvePoints = curve.points;
            return reply;
        } catch (error) {
            instance = undefined;
            return { error: String(error).slice(0, 512) };
        }
    };
}

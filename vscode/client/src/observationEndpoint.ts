import type { NodeAddress } from "./graphScope";
import type { ObservationCapability, ObservationHostRequest, ObservationInterest, ObservationResponse } from "./observations";

export type ObservationTransport = {
    describeObservations(nodes: NodeAddress[]): Promise<{ capabilities: ObservationCapability[] }>;
    reconcileObservations(owner: string, interests: ObservationInterest[],
        predictedInterests?: ObservationInterest[]): Promise<{ revision: string }>;
};
export type ObservationView = { visible: boolean; active: boolean };

export async function answerObservationRequest(transport: ObservationTransport, view: ObservationView,
    owner: string, request: ObservationHostRequest): Promise<ObservationResponse> {
    const reply = { type: "observationResponse" as const, requestId: request.requestId };
    try {
        const result = request.type === "observationDescribe" ? await transport.describeObservations(request.nodes)
            : await transport.reconcileObservations(owner, view.visible ? request.interests : [],
                view.visible && view.active ? request.predictedInterests : undefined);
        return { ...reply, result };
    } catch (error) {
        return { ...reply, error: String(error) };
    }
}

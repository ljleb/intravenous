import type { GraphScope, NodeAddress } from "./graphScope";
import type { ManualControlAction } from "./manualControls";
import type { NodeEventHostRequest } from "./nodeEventMessaging";
import type { ObservationView } from "./observationEndpoint";
import { PresentationObservations, type ObservationBaseline, type ObservationInput } from "./presentationObservations";

export type JsonRpcSend = { method: string; params: Record<string, unknown>; notification: boolean };
export type ClientInput =
    | { type: "observation"; input: ObservationInput }
    | { type: "manual"; action: ManualControlAction }
    | { type: "nodeEvent"; request: NodeEventHostRequest }
    | ({ type: "view" } & ObservationView)
    | { type: "expanded"; nodes: NodeAddress[] };
export type Timed<T> = { atMs: number; value: T };
export type InteractionRecording = {
    startedAtMs: number;
    baseline: ObservationBaseline;
    events: Timed<ClientInput>[];
    truncated: boolean;
};
export type ClientInteractionTrace = InteractionRecording & {
    version: 1;
    durationMs: number;
    view: ObservationView;
    expanded: NodeAddress[];
    playback: { position: string; running: boolean } | null;
    owner: string;
    sent: Timed<JsonRpcSend>[];
};

export class ClientInteractionRecorder {
    private readonly started = performance.now();
    private readonly events: Timed<ClientInput>[] = [];
    private truncated = false;
    private readonly baseline: ObservationBaseline;

    constructor(private readonly observations: PresentationObservations) {
        this.baseline = JSON.parse(JSON.stringify(observations.startRecording(
            (input) => this.record({ type: "observation", input }))));
    }

    record(value: ClientInput): void {
        if (this.events.length >= 8192) { this.truncated = true; return; }
        // Addresses observed in the webview may be Vue proxies.
        this.events.push({ atMs: performance.now() - this.started, value: JSON.parse(JSON.stringify(value)) });
    }

    stop(): InteractionRecording {
        this.observations.stopRecording();
        return { startedAtMs: performance.timeOrigin + this.started, baseline: this.baseline,
            events: this.events, truncated: this.truncated };
    }
}

export class ObservationTraceInput {
    private readonly owners = new Map<number, symbol>();
    constructor(private readonly observations: PresentationObservations) {}

    apply(input: ObservationInput): void {
        const observations = this.observations;
        switch (input.type) {
        case "lane": {
            let owner = this.owners.get(input.owner);
            if (owner === undefined) { owner = Symbol(); this.owners.set(input.owner, owner); }
            observations.node(owner, input.node, input.tier, input.window, input.firstInput);
            break;
        }
        case "removeLane": {
            const owner = this.owners.get(input.owner);
            if (owner !== undefined) observations.remove(owner);
            this.owners.delete(input.owner);
            break;
        }
        case "predict": observations.predictViewport(input.nodes); break;
        case "port": observations.observePort(input.id, input.observation, input.scheduling, input.consumer); break;
        case "visible": observations.setVisible(input.visible); break;
        case "refresh": observations.refresh(); break;
        case "disconnect": observations.disconnect(); break;
        }
    }
}

// The renderer needs a new capability discovery when a generation appears
// or becomes prepared. Render-only updates can keep the current interests.
export function observationConfiguration(scope: GraphScope | null): string {
    const generation = scope?.execution?.phase === "configurationFailed" ? null : scope?.generation ?? null;
    return JSON.stringify([generation, generation !== null && scope?.execution?.preparedGeneration === generation]);
}

export function isInteractionSend({ method, params }: JsonRpcSend, owner: string): boolean {
    if (method === "observations.reconcile") return params.scopeId === owner;
    if (method === "observations.describe" || method === "manualControls.hover" || method.startsWith("nodeMessages.")) return true;
    return method.startsWith("manualControls.") && typeof params.interactionId === "string"
        && params.interactionId.startsWith(JSON.stringify([owner]).slice(0, -1) + ",");
}

function normalizeSend({ method, params, notification }: JsonRpcSend, owner: string): JsonRpcSend {
    const normalized = { ...params };
    if (params.scopeId === owner) normalized.scopeId = "trace";
    if (method.startsWith("manualControls.") && typeof params.interactionId === "string") {
        const [, gesture] = JSON.parse(params.interactionId) as [string, string];
        normalized.interactionId = gesture;
    }
    return { method, params: normalized, notification };
}

export function compareInteractionSends(expected: Timed<JsonRpcSend>[], originalOwner: string,
    actual: Timed<JsonRpcSend>[], replayOwner: string) {
    const matched = Math.min(expected.length, actual.length);
    let firstDifference = 0;
    while (firstDifference < matched
        && JSON.stringify(normalizeSend(expected[firstDifference].value, originalOwner))
            === JSON.stringify(normalizeSend(actual[firstDifference].value, replayOwner))) ++firstDifference;
    const maximumTimingDifferenceMs = expected.slice(0, firstDifference).reduce(
        (largest, send, i) => Math.max(largest, Math.abs(actual[i].atMs - send.atMs)), 0);
    return { expectedCount: expected.length, actualCount: actual.length, matchingPrefix: firstDifference, maximumTimingDifferenceMs,
        difference: firstDifference === expected.length && firstDifference === actual.length ? null : {
            expected: expected[firstDifference] ?? null, actual: actual[firstDifference] ?? null,
        } };
}

// Temporary knob-audio recorder. The server records its audio callback output
// with per-block steady-clock times; the client records when it sent each gesture
// message. One shared clock offset, measured around the capture start request,
// places both on the server's steady clock for offline analysis.

export type AudioOutputCaptureReply = {
    recording: boolean;
    path: string;
    sampleRate: number;
    channels: number;
    sampleCount: number;
    // [sample offset, frame count, server steady microseconds at handoff]
    blocks: Array<[number, number, number]>;
    serverTimeMs: number;
    serverSteadyUs: number;
};

export type KnobGestureEvent = {
    kind: "begin" | "update" | "end";
    interactionId: string;
    inputName: string;
    value: number;
    // performance.now() when the message was handed to the socket.
    clientMs: number;
};

export type KnobPointerEvent = {
    kind: string;
    // Browser and extension clocks share epoch milliseconds, unlike their
    // independent performance.now() origins.
    epochMs: number;
    details: Record<string, unknown>;
};

export class KnobPointerTrace {
    private events: KnobPointerEvent[] = [];
    private dropped = 0;
    private lastMoveMs = -Infinity;
    constructor(private readonly limit = 2048) {}
    record(kind: string, epochMs: number, details: () => Record<string, unknown>): void {
        if (kind === "move" && epochMs - this.lastMoveMs < 30) return;
        if (kind === "move") this.lastMoveMs = epochMs;
        if (this.events.length >= this.limit) { ++this.dropped; return; }
        this.events.push({ kind, epochMs, details: details() });
    }
    snapshot() { return { events: this.events.slice(), dropped: this.dropped }; }
}

// Maps client performance.now() milliseconds to server steady microseconds,
// assuming the server read its clock halfway through the start request.
export function clientToServerClock(requestStartMs: number, requestEndMs: number, serverSteadyUs: number) {
    const midpointMs = (requestStartMs + requestEndMs) / 2;
    return {
        uncertaintyUs: (requestEndMs - requestStartMs) * 500,
        toServerUs: (clientMs: number) => serverSteadyUs + (clientMs - midpointMs) * 1000,
    };
}

export function knobAudioRecord(
    capture: AudioOutputCaptureReply,
    events: KnobGestureEvent[],
    clock: ReturnType<typeof clientToServerClock>,
) {
    return {
        format: "intravenous knob-audio recording",
        wav: capture.path,
        sampleRate: capture.sampleRate,
        channels: capture.channels,
        sampleCount: capture.sampleCount,
        clockUncertaintyUs: clock.uncertaintyUs,
        blocks: capture.blocks.map(([offset, frames, steadyUs]) => ({ offset, frames, steadyUs })),
        gestures: events.map((event) => ({
            kind: event.kind,
            interactionId: event.interactionId,
            inputName: event.inputName,
            value: event.value,
            steadyUs: Math.round(clock.toServerUs(event.clientMs)),
        })),
    };
}

export function recordingPathFor(wavPath: string): string {
    return wavPath.replace(/\.wav$/i, "") + ".json";
}

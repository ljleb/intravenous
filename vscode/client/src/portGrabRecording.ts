export type PortGrabEvent = {
    type: string;
    focused: boolean;
    target?: string;
    port?: string;
    pointerId?: number;
    button?: number;
    buttons?: number;
    x?: number;
    y?: number;
    capture?: boolean;
    gesture?: boolean;
    moved?: boolean;
    reason?: string;
};
export type PortGrabRecording = {
    startedAt: string;
    userAgent: string;
    result: "grab-observed" | "pointerdown-not-observed" | "no-pointer-attempt-observed"
        | "press-not-on-port" | "gesture-not-started" | "gesture-cancelled" | "no-drag-motion";
    observedWindowBlur: boolean;
    observedWindowFocus: boolean;
    truncated: boolean;
    events: Array<PortGrabEvent & { elapsedMs: number }>;
};

// Records actual webview input. A passing recording demonstrates a grab, not
// a backend graph mutation. It does not simulate native window activation.
export class PortGrabRecorder {
    private recording: Omit<PortGrabRecording, "result"> | null = null;
    private started = 0;
    private down = false;
    private downOnPort = false;
    private heldMove = false;
    private released = false;
    private began = false;
    private moved = false;
    private cancelled = false;
    private lastMove = "";

    constructor(private readonly now: () => number = () => performance.now()) {}
    get active(): boolean { return this.recording !== null; }

    start(userAgent: string): void {
        this.recording = { startedAt: new Date().toISOString(), userAgent,
            observedWindowBlur: false, observedWindowFocus: false, truncated: false, events: [] };
        this.started = this.now();
        this.down = this.downOnPort = this.heldMove = this.released = this.began = this.moved = this.cancelled = false;
        this.lastMove = "";
    }

    record(event: PortGrabEvent): void {
        const recording = this.recording;
        if (!recording) return;
        if (event.type === "blur") recording.observedWindowBlur = true;
        if (event.type === "focus") recording.observedWindowFocus = true;
        if (event.type === "pointerdown" && event.button === 0 && !this.down) {
            this.down = true; this.downOnPort = !!event.port;
        }
        if (event.type === "pointermove" && ((event.buttons ?? 0) & 1)) this.heldMove = true;
        if ((event.type === "pointerup" || event.type === "mouseup") && event.button === 0) this.released = true;
        if (event.type === "gesture-start") this.began = true;
        if (event.type === "gesture-move") this.moved = true;
        if (event.type === "gesture-cancel" && this.began && event.reason !== "release") this.cancelled = true;
        if (event.type === "pointermove") {
            const key = JSON.stringify([event.buttons, event.capture, event.gesture, event.moved, event.focused, event.target, event.port]);
            if (key === this.lastMove) return;
            this.lastMove = key;
        }
        if (recording.events.length === 256) {
            recording.events.splice(128, 1); // Preserve initial input and the latest events.
            recording.truncated = true;
        }
        recording.events.push({ ...event, elapsedMs: Math.round((this.now() - this.started) * 100) / 100 });
    }

    finish(): PortGrabRecording | null {
        if (!this.recording) return null;
        const result: PortGrabRecording["result"] = !this.down
            ? this.heldMove || this.released ? "pointerdown-not-observed" : "no-pointer-attempt-observed"
            : !this.downOnPort ? "press-not-on-port"
            : !this.began ? "gesture-not-started"
            : this.cancelled ? "gesture-cancelled"
            : this.moved ? "grab-observed" : "no-drag-motion";
        const report = { ...this.recording, result };
        this.recording = null;
        return report;
    }
}

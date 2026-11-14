export type KnobPointerTraceEvent = { timeMs: number; type: string; details: Record<string, unknown> };
export type KnobPointerTraceReport = {
    formatVersion: 1;
    armedAt: string;
    reason: string;
    environment: Record<string, unknown>;
    control: unknown;
    droppedEvents: number;
    events: KnobPointerTraceEvent[];
};

// Keep the acquisition history and a bounded tail of recent motion. Long
// reproductions retain the events next to the failure without growing forever.
export class KnobPointerTrace {
    private recording: { armedAt: string; environment: Record<string, unknown>; control: unknown;
        started: boolean; prefix: KnobPointerTraceEvent[]; tail: KnobPointerTraceEvent[];
        tailIndex: number; total: number; origin: number } | null = null;
    constructor(private readonly finished: (report: KnobPointerTraceReport) => void,
        private readonly now: () => number, private readonly capacity = 5000) {
        if (capacity < 2) throw new Error("A pointer trace needs room for at least two events");
    }
    armed(): boolean { return this.recording !== null; }
    capturing(): boolean { return this.recording?.started ?? false; }
    arm(environment: Record<string, unknown>, armedAt = new Date().toISOString()): void {
        this.recording = { armedAt, environment, control: null, started: false,
            prefix: [], tail: [], tailIndex: 0, total: 0, origin: this.now() };
    }
    begin(control: unknown, details: Record<string, unknown>): void {
        if (!this.recording || this.recording.started) return;
        this.recording.control = control;
        this.recording.started = true;
        this.recording.origin = this.now();
        this.record("drag-start", details);
    }
    record(type: string, details: Record<string, unknown>): void {
        const recording = this.recording;
        if (!recording?.started) return;
        const event = { type, details, timeMs: this.now() - recording.origin };
        ++recording.total;
        const prefixCapacity = Math.min(64, Math.floor(this.capacity / 2));
        if (recording.prefix.length < prefixCapacity) recording.prefix.push(event);
        else {
            const tailCapacity = this.capacity - prefixCapacity;
            if (recording.tail.length < tailCapacity) recording.tail.push(event);
            else { recording.tail[recording.tailIndex] = event; recording.tailIndex = (recording.tailIndex + 1) % tailCapacity; }
        }
    }
    finish(reason: string, details: Record<string, unknown> = {}): void {
        const recording = this.recording;
        if (!recording) return;
        this.record("trace-finished", details);
        this.recording = null;
        const tail = [...recording.tail.slice(recording.tailIndex), ...recording.tail.slice(0, recording.tailIndex)];
        const events = [...recording.prefix, ...tail];
        this.finished({ formatVersion: 1, armedAt: recording.armedAt, environment: recording.environment,
            control: recording.control, reason, events, droppedEvents: recording.total - events.length });
    }
}

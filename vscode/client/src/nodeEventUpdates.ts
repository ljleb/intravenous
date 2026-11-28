export type NodeEventUpdateTimers = { set(callback: () => void): unknown; clear(handle: unknown): void };

export const nodeEventUpdateIntervalMs = 80;
export const nodeEventUpdateTimers: NodeEventUpdateTimers = {
    set: (callback) => setTimeout(callback, nodeEventUpdateIntervalMs),
    clear: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>),
};

// Presentations keep the latest draft. Schedule one dispatch, then catch up
// to the newest value after each accepted update. Release flushes the draft.
export class NodeEventUpdates {
    private timer?: unknown;
    private pending?: Promise<void>;
    private stopped = false;

    constructor(private readonly sendNext: () => Promise<boolean>, private readonly onError: (error: unknown) => void | Promise<void>,
        private readonly timers = nodeEventUpdateTimers) {}

    schedule(): void {
        if (this.stopped || this.timer !== undefined || this.pending) return;
        this.timer = this.timers.set(() => { void this.flush().catch(() => {}); });
    }

    clearTimer(): void {
        if (this.timer !== undefined) this.timers.clear(this.timer);
        this.timer = undefined;
    }

    flush(): Promise<void> {
        if (this.pending) return this.pending;
        this.clearTimer();
        this.pending = (async () => {
            while (!this.stopped && await this.sendNext()) { /* coalesce updates during dispatch */ }
        })().catch(async (error) => {
            this.stopped = true;
            await this.onError(error);
            throw error;
        }).finally(() => { this.pending = undefined; });
        return this.pending;
    }

    stop(): Promise<void> {
        this.stopped = true;
        this.clearTimer();
        return this.pending ?? Promise.resolve();
    }
}

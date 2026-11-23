import { nodeEventUpdateTimers, type NodeEventUpdateTimers } from "./nodeEventUpdates";
import type { PresentationCommand } from "./presentationPlugin";

/** Pace full-state previews and preserve the order of all other edits. */
export class PresentationEdits {
    private readonly queued: PresentationCommand[] = [];
    private timer?: unknown;
    private sending = false;
    private closed = false;

    constructor(private readonly send: (command: PresentationCommand) => Promise<void>,
        private readonly changed: () => void, private readonly fail: (error: unknown) => void,
        private readonly timers: NodeEventUpdateTimers = nodeEventUpdateTimers) {}

    get pending(): boolean { return this.sending || this.queued.length > 0; }

    enqueue(command: PresentationCommand): void {
        if (this.closed) return;
        const previous = this.queued.at(-1);
        if (previous?.type === "edit" && previous.replacePending && command.type === "edit" && command.replacePending
            && previous.gesture === command.gesture && previous.event.eventType.nominalId === command.event.eventType.nominalId
            && previous.event.eventType.definitionFingerprint === command.event.eventType.definitionFingerprint) {
            this.queued[this.queued.length - 1] = command;
        } else {
            if (this.queued.length >= 128) throw new Error("Presentation edit queue exceeded its limit");
            this.queued.push(command);
        }
        this.changed();
        this.schedule();
    }

    private schedule(): void {
        if (this.closed || this.sending || this.queued.length === 0) return;
        const head = this.queued[0];
        if (this.queued.length === 1 && head.type === "edit" && head.replacePending) {
            this.timer ??= this.timers.set(() => {
                this.timer = undefined;
                void this.sendNext();
            });
        } else {
            this.clearTimer();
            void this.sendNext();
        }
    }

    private async sendNext(): Promise<void> {
        const head = this.queued.shift();
        if (!head || this.closed) return;
        this.sending = true;
        try { await this.send(head); }
        catch (error) {
            this.queued.length = 0;
            if (!this.closed) this.fail(error);
        } finally {
            this.sending = false;
            this.changed();
            this.schedule();
        }
    }

    private clearTimer(): void {
        if (this.timer !== undefined) this.timers.clear(this.timer);
        this.timer = undefined;
    }

    discardPending(): PresentationCommand[] {
        this.clearTimer();
        const discarded = this.queued.splice(0);
        this.changed();
        return discarded;
    }

    dispose(): void {
        this.closed = true;
        this.clearTimer();
        this.queued.length = 0;
    }
}

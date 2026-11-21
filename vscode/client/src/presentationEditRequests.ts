import { serializeNodeEvent, type NodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { nodeEventUpdateIntervalMs, type NodeEventUpdateTimers } from "./nodeEventUpdates";
import type { PresentationCommand, PresentationInput } from "./presentationPlugin";

type Reply = Extract<PresentationInput, { type: "editResult" }>;
type Request = Exclude<PresentationCommand, { type: "state" }>;

// A lane can wait for ordered node edits without knowing the host's RPC
// transport. The next frame carries any command that becomes ready.
export class PresentationEditRequests {
    private sequence = 0;
    private commands: Request[] = [];
    private readonly pending = new Map<string, (reply: Reply) => void>();

    send(event: NodeEvent): Promise<void> {
        return this.request({ type: "edit", event: serializeNodeEvent(event) }).then(() => {});
    }

    gesture(): NodeEventGesture {
        const gesture = `gesture:${++this.sequence}`;
        let used = false;
        let revision: number | undefined;
        let finished: Promise<void> | undefined;
        return {
            send: async (event) => {
                if (finished) throw new Error("Presentation gesture ended");
                used = true;
                revision = await this.request({ type: "edit", event: serializeNodeEvent(event), gesture }) ?? revision;
            },
            finish: () => finished ??= used ? this.request({ type: "finish", gesture }).then(() => {}) : Promise.resolve(),
            acceptedRevision: () => revision,
        };
    }

    receive(reply: Reply): void {
        const resolve = this.pending.get(reply.requestId);
        if (!resolve) return;
        this.pending.delete(reply.requestId);
        resolve(reply);
    }

    takeCommands(): Request[] {
        const ready = this.commands;
        this.commands = [];
        return ready;
    }

    private request(command: Request): Promise<number | undefined> {
        const requestId = `edit:${++this.sequence}`;
        this.commands.push({ ...command, requestId });
        return new Promise((resolve, reject) => this.pending.set(requestId, (reply) => {
            if (reply.error) reject(new Error(reply.error));
            else resolve(reply.acceptedRevision);
        }));
    }
}

// The host supplies time and decides when to render. Replay also works in a
// host without browser timers.
export class PresentationEditClock implements NodeEventUpdateTimers {
    private now = 0;
    private readonly callbacks = new Map<unknown, { at: number; run: () => void }>();

    set(run: () => void): unknown {
        const handle = {};
        this.callbacks.set(handle, { at: this.now + nodeEventUpdateIntervalMs, run });
        return handle;
    }

    clear(handle: unknown): void { this.callbacks.delete(handle); }

    setTime(now: number): void { this.now = Math.max(now, this.now); }

    runDue(): void {
        for (const [handle, { at, run }] of this.callbacks) {
            if (at > this.now) continue;
            this.callbacks.delete(handle);
            run();
        }
    }

    nextDelay(): number | undefined {
        let next: number | undefined;
        for (const { at } of this.callbacks.values()) if (next === undefined || at < next) next = at;
        return next === undefined ? undefined : Math.max(0, next - this.now);
    }
}

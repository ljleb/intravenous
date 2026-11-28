import { presentationNodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { PresentationEdits } from "./presentationEdits";
import { interruptedPresentationEditReplies, type PresentationCommand, type PresentationInput } from "./presentationPlugin";

type LaneEditHost = {
    editable(): boolean;
    gesture(): NodeEventGesture;
    accepted(revision: number): void;
    input(input: PresentationInput): void;
    setError(error: string): void;
    changed(): void;
    fail(error: unknown): void;
};

export class PresentationLaneEdits {
    private readonly gestures = new Map<string, NodeEventGesture>();
    private running?: { command: PresentationCommand; interrupted: boolean };
    readonly queue: PresentationEdits;

    constructor(private readonly host: LaneEditHost) {
        this.queue = new PresentationEdits((edit) => this.send(edit), host.changed, (reason) => {
            host.fail(reason);
            this.closeGestures();
        });
    }

    accept(commands: readonly PresentationCommand[] | undefined, allowNewEdits = true): void {
        for (const command of commands ?? []) {
            if (command.type === "finish" || (allowNewEdits && this.host.editable())) {
                // A guest can be waiting for a finish reply even after the
                // host interrupted and closed its gesture.
                this.queue.enqueue(command);
            } else if (command.requestId) {
                this.host.input({ type: "editResult", requestId: command.requestId, error: allowNewEdits
                    ? "The presentation node is no longer editable" : "This frame cannot submit node edits" });
            }
        }
    }

    interrupt(): PresentationInput[] {
        // History and disconnect invalidate the client handles even if the
        // guest already recorded a finish that the session will discard.
        const discarded = this.queue.discardPending();
        if (this.running && !this.running.interrupted) {
            this.running.interrupted = true;
            discarded.unshift(this.running.command);
        }
        void this.closeGestures();
        return interruptedPresentationEditReplies(discarded);
    }

    closeGestures(): Promise<void> {
        const finishing = [...this.gestures.values()].map((gesture) => gesture.finish().catch(() => {}));
        this.gestures.clear();
        return Promise.all(finishing).then(() => {});
    }

    private async send(command: PresentationCommand): Promise<void> {
        const running = { command, interrupted: false };
        this.running = running;
        try {
            const acceptedRevision = await this.sendNodeEdit(command);
            if (!running.interrupted && command.requestId)
                this.host.input({ type: "editResult", requestId: command.requestId, acceptedRevision });
        } catch (reason) {
            if (running.interrupted) return;
            if (!command.requestId) throw reason;
            const detail = reason instanceof Error ? reason.message : String(reason);
            this.running = undefined;
            this.host.input({ type: "editResult", requestId: command.requestId, error: detail.slice(0, 512) });
            if (command.type === "edit" && command.gesture) {
                const gesture = this.gestures.get(command.gesture);
                this.gestures.delete(command.gesture);
                await gesture?.finish().catch(() => {});
            }
        } finally { this.running = undefined; }
    }

    private async sendNodeEdit(command: PresentationCommand): Promise<number | undefined> {
        if (command.type === "finish") {
            const gesture = this.gestures.get(command.gesture);
            if (!gesture) return;
            this.gestures.delete(command.gesture);
            await gesture.finish();
            return gesture.acceptedRevision?.();
        }
        if (!this.host.editable()) throw new Error("The presentation node is no longer editable");
        this.host.setError("");
        const key = command.gesture;
        let gesture = key ? this.gestures.get(key) : undefined;
        if (!gesture) {
            if (key && this.gestures.size >= 8) throw new Error("Too many open presentation gestures");
            gesture = this.host.gesture();
            if (key) this.gestures.set(key, gesture);
        }
        try {
            await gesture.send(presentationNodeEvent(command.event));
            const revision = gesture.acceptedRevision?.();
            // The project can accept an edit while its presentation is being
            // interrupted. Hosts still need to read that committed revision.
            this.host.accepted(revision ?? 0);
            return revision;
        } finally { if (!key) await gesture.finish(); }
    }
}

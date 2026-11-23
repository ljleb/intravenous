import { presentationNodeEvent } from "./nodeEvents";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { PresentationEdits, type PresentationEdit } from "./presentationEdits";
import type { PresentationFrame, PresentationInput, PresentationSavedState } from "./presentationPlugin";

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
    readonly queue: PresentationEdits;

    constructor(private readonly host: LaneEditHost) {
        this.queue = new PresentationEdits((edit) => this.send(edit), host.changed, (reason) => {
            host.fail(reason);
            this.closeGestures();
        });
    }

    accept(frame: PresentationFrame, save: (state: PresentationSavedState) => void): void {
        for (const command of frame.commands ?? []) {
            if (command.type === "state") save(command.value);
            else if (command.type === "finish" || this.host.editable() || command.requestId) this.queue.enqueue(command);
        }
    }

    closeGestures(): Promise<void> {
        const finishing = [...this.gestures.values()].map((gesture) => gesture.finish().catch(() => {}));
        this.gestures.clear();
        return Promise.all(finishing).then(() => {});
    }

    private async send(command: PresentationEdit): Promise<void> {
        try {
            const acceptedRevision = await this.sendNodeEdit(command);
            if (command.requestId) this.host.input({ type: "editResult", requestId: command.requestId, acceptedRevision });
        } catch (reason) {
            if (!command.requestId) throw reason;
            const detail = reason instanceof Error ? reason.message : String(reason);
            this.host.input({ type: "editResult", requestId: command.requestId, error: detail.slice(0, 512) });
            if (command.type === "edit" && command.gesture) {
                const gesture = this.gestures.get(command.gesture);
                this.gestures.delete(command.gesture);
                await gesture?.finish().catch(() => {});
            }
        }
    }

    private async sendNodeEdit(command: PresentationEdit): Promise<number | undefined> {
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
            this.host.accepted(revision ?? 0);
            return revision;
        } finally { if (!key) await gesture.finish(); }
    }
}

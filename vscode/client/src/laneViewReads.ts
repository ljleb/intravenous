import type { SerializedPresentationState } from "./presentationStateJson";
import { presentationStateKey, type ProjectLaneView } from "./graphScope";

export type PresentationStateRead =
    | { status: "pending" }
    | { status: "ready"; state: SerializedPresentationState }
    | { status: "failed"; error: string };
type LaneViewRead = Exclude<PresentationStateRead, { status: "pending" }>
    | { status: "pending"; requestId: number; attempt: number };

export class LaneViewReads {
    private nextRequest = 0;

    constructor(private readonly request: (version: string, requestId: number) => void,
        private readonly reads = new Map<string, LaneViewRead>()) {}

    read(version: string): PresentationStateRead {
        return this.reads.get(version) ?? this.start(version, 1);
    }

    receive(version: string, requestId: number, state: string | null): void {
        const reading = this.reads.get(version);
        if (reading?.status !== "pending" || reading.requestId !== requestId) return;
        if (state !== null) this.reads.set(version, { status: "ready", state });
        else if (reading.attempt === 1) this.start(version, 2);
        else this.reads.set(version, { status: "failed", error: "Could not load saved lane state" });
    }

    rememberCommitted(submitted: Record<string, SerializedPresentationState>, references: readonly ProjectLaneView[],
        currentVersions: ReadonlyMap<string, string>): void {
        for (const { address, presentationId, version } of references) {
            const key = presentationStateKey(address, presentationId);
            const state = submitted[key];
            if (typeof state === "string" && currentVersions.get(key) === version)
                this.reads.set(version, { status: "ready", state });
        }
    }

    retain(versions: ReadonlySet<string>): void {
        for (const version of this.reads.keys()) if (!versions.has(version)) this.reads.delete(version);
    }

    retryUnread(): void {
        for (const [version, read] of this.reads) if (read.status !== "ready") this.reads.delete(version);
    }

    clear(): void { this.reads.clear(); }

    private start(version: string, attempt: number): LaneViewRead {
        const read = { status: "pending" as const, requestId: ++this.nextRequest, attempt };
        this.reads.set(version, read);
        this.request(version, read.requestId);
        return read;
    }
}

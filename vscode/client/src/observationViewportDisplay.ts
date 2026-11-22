import { compareObservationWindowVersions, observationWindowVersion, sameObservationWindow, type ObservationTimeSpan,
    type ObservationWindow, type ObservationWindowResult, type ObservationWindowVersion } from "./observations";
import { observationSpans, type ObservationSpan } from "./observationWindows";

export type ObservationViewportRead = {
    id: string;
    visible: ObservationTimeSpan;
    fine: readonly ObservationWindow[];
    ready: readonly ObservationWindowResult[];
    current: readonly (ObservationWindowResult | undefined)[];
    completeOnly: boolean;
};

type ObservationViewportPlot = {
    result?: ObservationWindowResult;
    spans: ObservationSpan[];
    status: ObservationWindowResult["status"];
    error?: string;
};

type Snapshot = { version: ObservationWindowVersion; windows: Map<string, ObservationWindowResult[]> };

function coversViewport(spans: readonly ObservationSpan[], visible: ObservationTimeSpan): boolean {
    let end = 0n;
    for (const span of spans) {
        if (span.begin !== end) return false;
        end = span.end;
    }
    return end === BigInt(visible.length);
}

function windowSpans(read: ObservationViewportRead, snapshot?: Snapshot) {
    const unavailable = read.current.flatMap((result) => result?.status === "unavailable" ? [result] : []);
    return observationSpans([...unavailable, ...(snapshot?.windows.get(read.id) ?? [])], read.visible, read.fine);
}

// A display has one published snapshot. Candidate windows can arrive in any
// order, but do not replace it until they answer every visible input range.
export class ObservationViewportDisplay {
    private shown?: Snapshot;
    private epoch?: number;

    clear(): void { this.shown = undefined; }

    read(reads: readonly ObservationViewportRead[], epoch: number): ObservationViewportPlot[] {
        if (this.epoch !== epoch) { this.shown = undefined; this.epoch = epoch; }
        if (!reads.length) { this.shown = undefined; return []; }
        const snapshots = new Map<string, Snapshot>();
        for (const read of reads) {
            for (const result of read.ready) {
                const key = observationWindowVersion(result);
                let snapshot = snapshots.get(key);
                if (!snapshot) snapshots.set(key, snapshot = { version: result, windows: new Map() });
                const retained = snapshot.windows.get(read.id);
                if (retained) retained.push(result);
                else snapshot.windows.set(read.id, [result]);
            }
        }
        if (this.shown) {
            const key = observationWindowVersion(this.shown.version);
            let snapshot = snapshots.get(key);
            if (!snapshot) snapshots.set(key, snapshot = { version: this.shown.version, windows: new Map() });
            for (const { id } of reads) {
                const windows = snapshot.windows.get(id) ?? [];
                for (const result of this.shown.windows.get(id) ?? [])
                    if (!windows.some((entry) => sameObservationWindow(entry.window, result.window))) windows.push(result);
                snapshot.windows.set(id, windows);
            }
        }

        const newer = [...snapshots.values()].filter(({ version }) =>
            !this.shown || compareObservationWindowVersions(version, this.shown.version) >= 0)
            .sort((a, b) => compareObservationWindowVersions(b.version, a.version));
        for (const snapshot of newer) {
            const spans = reads.map((read) => windowSpans(read, snapshot));
            if (!spans.every((parts, i) => coversViewport(parts, reads[i].visible))) continue;
            // Retain only the windows the display used. Newer candidates live
            // in ObservationValues under the ordinary cache limits.
            const { semanticVersion, dataVersion, executionGeneration } = snapshot.version;
            this.shown = { version: { semanticVersion, dataVersion, executionGeneration }, windows: new Map(reads.map(({ id }, i) =>
                [id, [...new Set(spans[i].map(({ result }) => result).filter(({ status }) => status === "ready"))]])) };
            return reads.map((read, i) => viewportPlot(read, spans[i], snapshot !== newer[0]));
        }
        const held = this.shown && snapshots.get(observationWindowVersion(this.shown.version));
        if (this.shown)
            for (const id of this.shown.windows.keys())
                if (!reads.some((read) => read.id === id)) this.shown.windows.delete(id);
        return reads.map((read) => viewportPlot(read, windowSpans(read, held), true));
    }
}

function viewportPlot(read: ObservationViewportRead, spans: ObservationSpan[], waiting = false): ObservationViewportPlot {
    const failure = read.current.find((result) => result?.status === "failed");
    const pending = read.current.some((result) => !result || result.status === "pending" || (read.completeOnly && result.truncated));
    const unavailable = read.current.every((result) => result?.status === "unavailable");
    const status = failure ? "failed" : unavailable ? "unavailable" : waiting || pending ? "pending" : "ready";
    const result = spans.find(({ result }) => result.status === "ready")?.result ?? spans[0]?.result ?? failure;
    return { result, spans, status, error: failure?.error };
}

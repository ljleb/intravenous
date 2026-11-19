import type { ObservationDelivery } from "./observationDelivery";

export class ObservationProfile {
    private readonly started = performance.now();
    private readonly encoder = new TextEncoder();
    private readonly counts = {
        updates: 0, presentationUpdates: 0, snapshots: 0, messageBytes: 0, measurementMs: 0,
        liveValues: 0, windowChanges: 0, windowBodies: 0, windowBytes: 0,
        statusChanges: 0, removals: 0, metadataOnly: 0,
    };

    record(message: { type: "observationUpdate"; update: ObservationDelivery }, presentationUpdated: boolean): void {
        const started = performance.now();
        const { update } = message;
        const counts = this.counts;
        ++counts.updates;
        if (update.snapshot) ++counts.snapshots;
        if (presentationUpdated) ++counts.presentationUpdates;
        counts.messageBytes += this.encoder.encode(JSON.stringify(message)).byteLength;
        const scopes = Object.values(update.scopes);
        if (!scopes.length) ++counts.metadataOnly;
        for (const scope of scopes) {
            if (!scope) {
                ++counts.removals;
                continue;
            }
            for (const value of Object.values(scope.values ?? {})) {
                if (value) ++counts.liveValues;
                else ++counts.removals;
            }
            for (const window of Object.values(scope.windows ?? {})) {
                if (!window) {
                    ++counts.removals;
                    continue;
                }
                ++counts.windowChanges;
                if (window.status !== undefined || window.outdated !== undefined) ++counts.statusChanges;
                if (window.body) {
                    ++counts.windowBodies;
                    counts.windowBytes += this.encoder.encode(JSON.stringify(window.body)).byteLength;
                }
            }
        }
        counts.measurementMs += performance.now() - started;
    }

    summary() {
        return { ...this.counts, seconds: (performance.now() - this.started) / 1000 };
    }
}

export type ObservationProfileSummary = ReturnType<ObservationProfile["summary"]>;

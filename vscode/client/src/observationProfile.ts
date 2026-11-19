import type { ObservationUpdate } from "./observations";

type ProfiledWindow = { body: string; geometry: string; dataVersion?: string };

export class ObservationProfile {
    private readonly started = performance.now();
    private readonly encoder = new TextEncoder();
    private readonly counts = {
        updates: 0, presentationUpdates: 0, messageBytes: 0, measurementMs: 0,
        unchangedScopes: 0, unchangedScopesWithNewDataVersion: 0,
        liveValues: 0, repeatedLiveValues: 0,
        readyWindows: 0, pendingWindows: 0, unavailableWindows: 0, failedWindows: 0, outdatedWindows: 0,
        repeatedWindows: 0, repeatedWindowsWithNewDataVersion: 0,
        windowBytes: 0, repeatedWindowBytes: 0,
    };
    private generation?: string;
    private previousScopes?: string;
    private previousDataVersion?: string;
    private previousValues = new Map<string, string>();
    private previousWindows = new Map<string, ProfiledWindow>();

    record(message: { type: "observationUpdate"; update: ObservationUpdate }, presentationUpdated: boolean): void {
        const started = performance.now();
        const { update } = message;
        const counts = this.counts;
        if (update.generation !== this.generation) {
            this.generation = update.generation;
            this.previousScopes = undefined;
            this.previousValues.clear();
            this.previousWindows.clear();
        }

        ++counts.updates;
        if (presentationUpdated) ++counts.presentationUpdates;
        counts.messageBytes += this.encoder.encode(JSON.stringify(message)).byteLength;
        const scopes = JSON.stringify(update.scopes);
        if (scopes === this.previousScopes) {
            ++counts.unchangedScopes;
            if (update.dataVersion !== this.previousDataVersion) ++counts.unchangedScopesWithNewDataVersion;
        }
        this.previousScopes = scopes;
        this.previousDataVersion = update.dataVersion;

        const values = new Map<string, string>();
        const windows = new Map<string, ProfiledWindow>();
        for (const scope of update.scopes) {
            for (const entry of scope.values) {
                const key = JSON.stringify([scope.scopeId, entry.id]);
                const data = JSON.stringify(entry);
                ++counts.liveValues;
                if (data === this.previousValues.get(key)) ++counts.repeatedLiveValues;
                values.set(key, data);
            }
            for (const result of scope.windows ?? []) {
                ++counts[`${result.status}Windows`];
                if (result.status !== "ready") continue;
                if (result.outdated) ++counts.outdatedWindows;
                const key = JSON.stringify([scope.scopeId, result.id, result.window]);
                const body = JSON.stringify([result.regions, result.marks, result.unavailableRegions]);
                const geometry = JSON.stringify([result.sampleRate, result.paceExponent, result.truncated, result.approximate]);
                const bytes = this.encoder.encode(body).byteLength;
                counts.windowBytes += bytes;
                const previous = this.previousWindows.get(key);
                if (previous?.body === body && previous.geometry === geometry) {
                    ++counts.repeatedWindows;
                    counts.repeatedWindowBytes += bytes;
                    if (result.dataVersion !== previous.dataVersion) ++counts.repeatedWindowsWithNewDataVersion;
                }
                windows.set(key, { body, geometry, dataVersion: result.dataVersion });
            }
        }
        this.previousValues = values;
        this.previousWindows = windows;
        counts.measurementMs += performance.now() - started;
    }

    summary() {
        return { ...this.counts, seconds: (performance.now() - this.started) / 1000 };
    }
}

export type ObservationProfileSummary = ReturnType<ObservationProfile["summary"]>;

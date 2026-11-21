import type { ObservationDelivery } from "./observationDelivery";
import { sameObservationWindow, type ObservationInterest, type ObservationValues, type ObservationWindow,
    type ObservationWindowResult } from "./observations";

type InterestProfile = {
    id: string; consumer: string; source: string; prefetch: boolean;
    active: boolean; atStart: boolean; requests: number; budgetSkips: number;
    windowChanges: number; requested?: ObservationWindow; received?: ObservationWindow; lastError?: string;
    live: { available: number; pending: number; unavailable: number };
    statuses: Partial<Record<ObservationWindowResult["status"], number>>;
    updated: number; bodies: number; bytes: number; points: number; marks: number;
    outdated: number; truncated: number; removals: number;
    eligibilityKey?: string;
    firstResult?: { eligible: number; submitted: number; usable?: number };
};
type InterestLabel = (interest: ObservationInterest) => Pick<InterestProfile, "consumer" | "source" | "eligibilityKey">;
const maximumProfileInterests = 1024;

export class ObservationProfile {
    private readonly started: number;
    private readonly encoder = new TextEncoder();
    private readonly interests = new Map<string, InterestProfile>();
    private readonly eligibility = new Map<string, number>();
    private active = new Set<string>();
    private readonly counts = {
        updates: 0, presentationUpdates: 0, snapshots: 0, messageBytes: 0, measurementMs: 0,
        liveValues: 0, windowChanges: 0, windowBodies: 0, windowBytes: 0,
        statusChanges: 0, removals: 0, metadataOnly: 0,
        reconcileRequests: 0, unattributedChanges: 0, omittedInterests: 0,
    };
    constructor(private readonly now = () => performance.now()) { this.started = now(); }

    recordEligibility(key: string, eligible: boolean): void {
        if (!eligible) this.eligibility.delete(key);
        else if (!this.eligibility.has(key) && this.eligibility.size < maximumProfileInterests)
            this.eligibility.set(key, this.now());
    }

    recordUsable(id: string): void {
        const entry = this.interests.get(id);
        const first = entry?.firstResult;
        if (entry?.active && entry.eligibilityKey !== undefined && first && first.usable === undefined
            && this.eligibility.get(entry.eligibilityKey) === first.eligible) first.usable = this.now();
    }

    private entry(interest: ObservationInterest, label: InterestLabel): InterestProfile | undefined {
        const current = this.interests.get(interest.id);
        if (current) return current;
        if (this.interests.size >= maximumProfileInterests) return;
        const entry: InterestProfile = { id: interest.id, ...label(interest), prefetch: !!interest.prefetch,
            active: false, atStart: false, requests: 0, budgetSkips: 0, windowChanges: 0,
            live: { available: 0, pending: 0, unavailable: 0 }, statuses: {},
            updated: 0, bodies: 0, bytes: 0, points: 0, marks: 0, outdated: 0, truncated: 0, removals: 0 };
        this.interests.set(interest.id, entry);
        return entry;
    }

    recordRequests(interests: readonly ObservationInterest[], label: InterestLabel, atStart = false): void {
        const started = this.now();
        if (!atStart) ++this.counts.reconcileRequests;
        for (const id of this.active) this.interests.get(id)!.active = false;
        this.active = new Set();
        for (const interest of interests) {
            const entry = this.entry(interest, label);
            if (!entry) { ++this.counts.omittedInterests; continue; }
            this.active.add(interest.id);
            entry.active = true;
            if (atStart) entry.atStart = true;
            else {
                ++entry.requests;
                const eligible = entry.eligibilityKey === undefined ? undefined : this.eligibility.get(entry.eligibilityKey);
                if (eligible !== undefined && eligible !== entry.firstResult?.eligible)
                    entry.firstResult = { eligible, submitted: started };
            }
            if (!sameObservationWindow(entry.requested, interest.window)) ++entry.windowChanges;
            entry.requested = interest.window ? { ...interest.window } : undefined;
        }
        this.counts.measurementMs += this.now() - started;
    }

    recordBudgetSkip(interest: ObservationInterest, label: InterestLabel): void {
        const entry = this.entry(interest, label);
        if (entry) {
            ++entry.budgetSkips;
            if (!entry.active && !entry.requests && !entry.atStart && interest.window) entry.requested = { ...interest.window };
        } else ++this.counts.omittedInterests;
    }

    record(message: { type: "observationUpdate"; update: ObservationDelivery }, presentationUpdated: boolean): void {
        const started = this.now();
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
            for (const [id, value] of Object.entries(scope.values ?? {})) {
                const entry = this.interests.get(id);
                if (!entry) ++counts.unattributedChanges;
                else if (value) ++entry.live[value.pending ? "pending" : value.available ? "available" : "unavailable"];
                else ++entry.removals;
                if (value) ++counts.liveValues;
                else ++counts.removals;
            }
            for (const [id, window] of Object.entries(scope.windows ?? {})) {
                const entry = this.interests.get(id);
                if (!entry) ++counts.unattributedChanges;
                if (!window) {
                    ++counts.removals;
                    if (entry) ++entry.removals;
                    continue;
                }
                if (entry) {
                    ++entry.updated;
                    if (window.status) entry.statuses[window.status] = (entry.statuses[window.status] ?? 0) + 1;
                    if (window.window) entry.received = window.window;
                    if (window.error) entry.lastError = window.error.slice(0, 240);
                    if (window.outdated) ++entry.outdated;
                    if (window.truncated) ++entry.truncated;
                }
                ++counts.windowChanges;
                if (window.status !== undefined || window.outdated !== undefined) ++counts.statusChanges;
                if (window.body) {
                    ++counts.windowBodies;
                    const bytes = this.encoder.encode(JSON.stringify(window.body)).byteLength;
                    counts.windowBytes += bytes;
                    if (entry) {
                        ++entry.bodies;
                        entry.bytes += bytes;
                        entry.marks += window.body.marks?.length ?? 0;
                        entry.points += window.body.regions.reduce((total, region) => total + region.points.length, 0);
                    }
                }
            }
        }
        counts.measurementMs += this.now() - started;
    }

    summary(values?: ObservationValues) {
        const seconds = (this.now() - this.started) / 1000;
        const interests = [...this.interests.values()].map(({ eligibilityKey, firstResult, ...entry }) => {
            const window = entry.active && entry.requested ? values?.windowForDisplay(entry.id, entry.requested) : undefined;
            const live = entry.active && !entry.requested ? values?.values.get(entry.id) : undefined;
            const cached = window ? {
                status: window.status, window: window.window, outdated: !!window.outdated, truncated: !!window.truncated,
                error: window.error?.slice(0, 240),
            } : live ? { status: live.pending ? "pending" : live.available ? "available" : "unavailable" } : undefined;
            return { ...entry, cached, firstResult: firstResult && {
                eligibilityToSubmitMs: firstResult.submitted - firstResult.eligible,
                submitToUsableMs: firstResult.usable === undefined ? undefined : firstResult.usable - firstResult.submitted,
            } };
        });
        return { ...this.counts, seconds, interests };
    }
}

export type ObservationProfileSummary = ReturnType<ObservationProfile["summary"]>;

function span(window: ObservationWindow): string {
    return `sample ${window.begin}, length ${window.length}, budget ${window.points}`
        + (window.maxDepth === undefined ? "" : `, depth ${window.maxDepth}`);
}

export function observationInterestReport(summary: ObservationProfileSummary): string[] {
    const lines = [`  ${summary.reconcileRequests} submitted interest sets, ${summary.unattributedChanges} changes without a tracked interest.`];
    if (summary.omittedInterests) lines.push(`  Profile capacity skipped ${summary.omittedInterests} interest entries.`);
    const consumers = new Map<string, ObservationProfileSummary["interests"]>();
    for (const entry of summary.interests) {
        const group = consumers.get(entry.consumer);
        if (group) group.push(entry);
        else consumers.set(entry.consumer, [entry]);
    }
    for (const [consumer, entries] of consumers) {
        lines.push(`  ${consumer}`);
        for (const entry of entries) {
            const request = [`${entry.requests} submissions`, entry.atStart ? "active at start" : "",
                entry.active ? "active at finish" : "", entry.budgetSkips ? `skipped by budget ${entry.budgetSkips} times` : ""]
                .filter(Boolean).join(", ");
            lines.push(`    ${entry.id} ${entry.source}${entry.prefetch ? " (prefetch)" : ""} — ${request}`);
            if (entry.firstResult) {
                const { eligibilityToSubmitMs, submitToUsableMs } = entry.firstResult;
                lines.push(`      First result on latest entry took ${eligibilityToSubmitMs.toFixed(2)} ms to submit from viewport eligibility, `
                    + (submitToUsableMs === undefined ? "with no usable reply recorded."
                        : `${submitToUsableMs.toFixed(2)} ms from submission to a usable reply.`));
            }
            if (entry.requested || entry.updated) {
                if (entry.requested) lines.push(entry.atStart || entry.requests
                    ? `      Last submitted window ${span(entry.requested)} (${entry.windowChanges} window versions).`
                    : `      Budget-skipped window ${span(entry.requested)}.`);
                const statuses = Object.entries(entry.statuses).map(([status, count]) => `${count} ${status}`).join(", ");
                lines.push(`      Received ${entry.updated} window patches, ${entry.bodies} bodies (${(entry.bytes / 1024).toFixed(2)} KiB, `
                    + `${entry.points} sample points, ${entry.marks} marks)${statuses ? `, status changes ${statuses}` : ""}.`);
                if (entry.outdated || entry.truncated) lines.push(`      ${entry.outdated} outdated flags, ${entry.truncated} truncated flags.`);
                if (entry.received) lines.push(`      Last window header ${span(entry.received)}.`);
            } else {
                lines.push(`      Received ${entry.live.available} available, ${entry.live.pending} pending, ${entry.live.unavailable} unavailable live readings.`);
            }
            if (entry.removals) lines.push(`      Received ${entry.removals} removals.`);
            if (entry.lastError) lines.push(`      Last window error ${entry.lastError}`);
            if (entry.cached) {
                const window = "window" in entry.cached ? entry.cached.window : undefined;
                lines.push(`      Cached at finish ${entry.cached.status}${entry.cached.outdated ? ", outdated" : ""}`
                    + `${entry.cached.truncated ? ", truncated" : ""}${window ? `, ${span(window)}` : ""}`
                    + `${entry.cached.error ? ` (${entry.cached.error})` : ""}.`);
            } else if (entry.active) lines.push("      No cached result at finish.");
        }
    }
    return lines;
}

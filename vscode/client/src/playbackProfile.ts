import type { GraphScope } from "./graphScope";
import type { ObservationDelivery } from "./observationDelivery";
import type { PlaybackOutputCutover, PlaybackOutputProfileResult, PlaybackSnapshot } from "./playback";

const timelineLimit = 120;

function sampleStep(before: number | null | undefined, after: number | null | undefined): number | null {
    return typeof before === "number" && typeof after === "number" ? Math.abs(after - before) : null;
}

function peakStep(samples: readonly (number | null)[], channel: number): number | null {
    let peak: number | null = null;
    for (let i = channel + 2; i < samples.length; i += 2) {
        const step = sampleStep(samples[i - 2], samples[i]);
        if (step !== null) peak = Math.max(peak ?? 0, step);
    }
    return peak;
}

function cutoverReport(cutover: PlaybackOutputCutover): string[] {
    const { before, after } = cutover;
    const amplitude = (sample: number | null): string => sample?.toPrecision(5) ?? "?";
    const stereo = (measure: (channel: number) => number | null): string => [0, 1].map(measure).map(amplitude).join(" / ");
    const step = stereo((channel) => sampleStep(before.at(channel - 2), after[channel]));
    const conditions = [
        cutover.transportChanged ? "transport changed" : "",
        !cutover.previousRendered || !cutover.rendered ? "Tick unavailable on one or both sides" : "",
        cutover.previousTransitionTick !== cutover.transitionTick
            ? cutover.transitionTick ? "entered transition Tick" : "left transition Tick"
            : "",
    ].filter(Boolean);
    return [
        `    +${(cutover.elapsedMs / 1000).toFixed(3)} s · foreground sample ${cutover.foregroundSample} · execution ${cutover.previousGeneration ?? "none"} → ${cutover.executionGeneration ?? "none"}, code publication ${cutover.previousPublication ?? "none"} → ${cutover.codePublication ?? "none"}`,
        `      Buffer render ${cutover.renderMs.toFixed(3)} / ${cutover.budgetMs.toFixed(3)} ms${conditions.length ? ` · ${conditions.join(", ")}` : ""} · step L/R ${step} · nearby peak step L/R ${stereo((channel) => peakStep(before, channel))} before, ${stereo((channel) => peakStep(after, channel))} after`,
        `      Stereo samples (L R) [${before.map(amplitude).join(" ")}] → [${after.map(amplitude).join(" ")}]`,
    ];
}

export class PlaybackProfile {
    private readonly timeline: string[] = [];
    private omitted = 0;
    private playback?: PlaybackSnapshot;
    private graph?: GraphScope["execution"];
    private observation?: Pick<ObservationDelivery, "revision" | "activationId">;
    private reads = 0;
    private missing = 0;
    private longestReadMs = 0;
    private underruns = 0n;
    private cutovers = 0;
    private sameConfigurationCutovers = 0;
    private readonly clickMarkers: number[] = [];
    private stoppedAt?: number;

    constructor(private readonly start: number) {}

    stop(now: number): void { this.stoppedAt ??= now; }

    private add(now: number, text: string): void {
        if (this.timeline.length === timelineLimit) {
            ++this.omitted;
            return;
        }
        this.timeline.push(`  +${((now - this.start) / 1000).toFixed(3)} s  ${text}`);
    }

    recordPlayback(snapshot: PlaybackSnapshot | null, sent: number, received: number): void {
        if (this.stoppedAt !== undefined) return;
        this.longestReadMs = Math.max(this.longestReadMs, received - sent);
        const diagnostics = snapshot?.diagnostics;
        if (!snapshot || !diagnostics) {
            ++this.missing;
            return;
        }
        ++this.reads;
        const previous = this.playback;
        const before = previous?.diagnostics;
        this.playback = snapshot;
        const sample = snapshot.executionIndex ?? snapshot.position;
        if (!previous || !before) {
            this.add(received, `Playback baseline · execution ${diagnostics.executionGeneration ?? "none"}, configuration ${diagnostics.configuredGeneration ?? "none"}, foreground sample ${sample}`);
            return;
        }
        if (diagnostics.executionGeneration !== before.executionGeneration) {
            const sameConfiguration = diagnostics.configuredGeneration === before.configuredGeneration
                && diagnostics.configuredGeneration !== null;
            ++this.cutovers;
            if (sameConfiguration) ++this.sameConfigurationCutovers;
            this.add(received, `Active execution ${before.executionGeneration ?? "none"} → ${diagnostics.executionGeneration ?? "none"}, configuration ${diagnostics.configuredGeneration ?? "none"}, foreground sample ${sample}`);
        }
        const delta = BigInt(diagnostics.backgroundUnderrunBlocks) - BigInt(before.backgroundUnderrunBlocks);
        if (delta < 0n) this.add(received, "Server underrun counter reset");
        if (delta > 0n) {
            this.underruns += delta;
            this.add(received, `Background underrun · ${delta} blocks silenced, foreground sample ${sample}`);
        }
        if (snapshot.segment !== previous.segment || snapshot.running !== previous.running) {
            this.add(received, `Transport · ${snapshot.running ? "playing" : "paused"}, segment ${snapshot.segment}, background sample ${snapshot.position}`);
        }
    }

    recordGraph(execution: GraphScope["execution"], now: number): void {
        if (this.stoppedAt !== undefined) return;
        if (!execution || (execution.phase === this.graph?.phase
            && execution.preparedGeneration === this.graph.preparedGeneration
            && execution.activeGeneration === this.graph.activeGeneration)) return;
        this.graph = execution;
        this.add(now, `Graph ${execution.phase} · prepared ${execution.preparedGeneration ?? "none"}, active ${execution.activeGeneration ?? "none"}`);
    }

    recordObservation({ revision, activationId }: Pick<ObservationDelivery, "revision" | "activationId">, now: number): void {
        if (this.stoppedAt !== undefined) return;
        if (revision === this.observation?.revision && activationId === this.observation.activationId) return;
        this.observation = { revision, activationId };
        this.add(now, `Live meter probes · requirement revision ${revision}, activation ${activationId}`);
    }

    markClick(now: number): void {
        if (this.stoppedAt === undefined) this.clickMarkers.push(now - this.start);
    }

    report(now: number, output?: PlaybackOutputProfileResult): string {
        const lines = [
            `Playback profile over ${(((this.stoppedAt ?? now) - this.start) / 1000).toFixed(2)} s`,
            `  Playback reads · ${this.reads} with diagnostics, ${this.missing} unavailable, longest read ${this.longestReadMs.toFixed(1)} ms`,
            `  Audible clicks marked · ${this.clickMarkers.length}${this.clickMarkers.length ? ` · ${this.clickMarkers.map((ms) => `+${(ms / 1000).toFixed(3)} s`).join(", ")} (extension time)` : ""}`,
        ];
        if (this.reads >= 2) {
            lines.push(`  Observed execution changes · ${this.cutovers}, including ${this.sameConfigurationCutovers} within the same graph configuration`);
            lines.push(`  Background underruns · ${this.underruns} blocks silenced between the first and last playback readings`);
        } else if (this.reads) {
            lines.push("  Two playback readings with diagnostics are required to measure cutovers and underruns.");
        } else {
            lines.push("  The server returned no playback diagnostics during the profile.");
        }
        if (output?.status === "ready") {
            lines.push(`  Device buffers · ${output.buffers} rendered, ${output.overBudgetBuffers} exceeded their audio duration · longest render ${output.maxRenderMs.toFixed(3)} ms, highest budget use ${(output.maxBudgetFraction * 100).toFixed(1)}%`);
            lines.push(`  Rendered output around graph switches · ${output.cutovers.length} captured, ${output.omittedCutovers} omitted (times relative to server capture start)`);
            for (const cutover of output.cutovers) lines.push(...cutoverReport(cutover));
        } else if (output) {
            lines.push(`  Device buffer capture unavailable (${output.status})`);
        }
        if (this.timeline.length) lines.push("  Timeline (times are when the extension received each update)", ...this.timeline);
        if (this.omitted) lines.push(`  ${this.omitted} later timeline entries omitted`);
        return lines.join("\n");
    }
}

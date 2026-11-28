export type PlaybackOutputProfileRequest = { id: string; durationMs?: number };
export type PlaybackOutputCutover = {
    previousGeneration: string | null;
    executionGeneration: string | null;
    previousPublication: string | null;
    codePublication: string | null;
    foregroundSample: string;
    previousRendered: boolean;
    rendered: boolean;
    previousTransitionTick: boolean;
    transitionTick: boolean;
    transportChanged: boolean;
    elapsedMs: number;
    renderMs: number;
    budgetMs: number;
    /** Chronological interleaved stereo samples. Non-finite samples are null. */
    before: (number | null)[];
    after: (number | null)[];
};
export type PlaybackOutputProfileResult = {
    status: "ready";
    buffers: number;
    overBudgetBuffers: number;
    omittedCutovers: number;
    maxRenderMs: number;
    maxBudgetFraction: number;
    cutovers: PlaybackOutputCutover[];
} | { status: "recording" | "busy" | "pending" | "unavailable" };

export type PlaybackSnapshot = {
    /** Applied background playhead as a signed decimal sample index. */
    position: string;
    startPosition?: string;
    /** Foreground playhead as a signed decimal sample index. */
    executionIndex?: string;
    running: boolean;
    foregroundRunning: boolean;
    sampleRate: number;
    appliedCommand: string;
    segment: string;
    acceptedCommand?: string;
    outputProfile?: PlaybackOutputProfileResult;
    diagnostics?: {
        backgroundUnderrunBlocks: string;
        configuredGeneration: string | null;
        executionGeneration: string | null;
    };
};

export type PlaybackSeek = { position: string; acceptedCommand?: string };
export type PlaybackDragPhase = "begin" | "move" | "end";
export type PlaybackDrag = { phase: PlaybackDragPhase; dragId: string };

export function isPendingPlaybackSeek(seek: PlaybackSeek, snapshot: PlaybackSnapshot | null): boolean {
    return seek.acceptedCommand === undefined || snapshot === null
        || BigInt(snapshot.appliedCommand) < BigInt(seek.acceptedCommand);
}

const minimum = -(1n << 63n), maximum = (1n << 63n) - 1n;
export function playbackSampleIndexFromSeconds(seconds: number, sampleRate: number): string {
    const samples = Math.round(seconds * sampleRate);
    if (!Number.isSafeInteger(samples))
        throw new Error("Seconds must resolve to an exact integer sample position; use a sample index for larger positions.");
    return samples.toString();
}

export function playbackSeekPosition(text: string, sampleRate: number): string {
    const trimmed = text.trim();
    if (/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)\s*s$/.test(trimmed)) {
        return playbackSampleIndexFromSeconds(Number(trimmed.slice(0, -1)), sampleRate);
    }
    if (!/^[+-]?\d+$/.test(trimmed)) throw new Error("Enter an integer sample position, or seconds followed by s.");
    const value = BigInt(trimmed);
    if (value < minimum || value > maximum) throw new Error("Sample position is outside the signed 64-bit range.");
    return value.toString();
}

// Keep exact backend positions intact. Animation adds only a small local
// offset and never crosses a pause/seek segment or runs indefinitely if updates
// stop. The displayed time estimates execution, not speaker presentation time.
const extrapolationLimitMs = 250;

export class PlaybackDisplay {
    private selected: PlaybackSnapshot | null = null;
    private receivedAt = 0;
    private backgroundAdvancing = false;
    private foregroundAdvancing = false;
    update(snapshot: PlaybackSnapshot | null, now: number): void {
        this.backgroundAdvancing = snapshot?.running === true && (snapshot.segment !== this.selected?.segment
            || snapshot.position !== this.selected?.position);
        const foregroundPlayhead = snapshot?.executionIndex;
        const previousForegroundPlayhead = this.selected?.executionIndex;
        this.foregroundAdvancing = snapshot?.foregroundRunning === true && foregroundPlayhead !== undefined && (snapshot.running ? this.backgroundAdvancing
            : previousForegroundPlayhead !== undefined && foregroundPlayhead !== previousForegroundPlayhead);
        this.selected = snapshot;
        this.receivedAt = now;
    }
    isAnimating(now: number): boolean {
        return (this.backgroundAdvancing || this.foregroundAdvancing) && now - this.receivedAt < extrapolationLimitMs;
    }
    backgroundPlayhead(now: number): bigint | null {
        return this.displayPosition(this.selected?.position, this.backgroundAdvancing, now);
    }
    foregroundPlayhead(now: number): bigint | null {
        return this.displayPosition(this.selected?.executionIndex, this.foregroundAdvancing, now);
    }
    private displayPosition(position: string | undefined, advancing: boolean, now: number): bigint | null {
        const state = this.selected;
        if (!state || position === undefined) return null;
        const elapsed = Math.max(0, Math.min(extrapolationLimitMs, now - this.receivedAt));
        const advance = advancing ? elapsed * state.sampleRate / 1000 : 0;
        return BigInt.asIntN(64, BigInt(position) + BigInt(Math.floor(advance)));
    }
}

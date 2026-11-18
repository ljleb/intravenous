export type PlaybackSnapshot = {
    position: string;
    startPosition?: string;
    executionIndex?: string;
    running: boolean;
    sampleRate: number;
    appliedCommand: string;
    segment: string;
    acceptedCommand?: string;
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
    private advancing = false;
    private executionAdvancing = false;
    update(snapshot: PlaybackSnapshot | null, now: number): void {
        this.advancing = snapshot?.running === true && (snapshot.segment !== this.selected?.segment
            || snapshot.position !== this.selected?.position);
        const executionIndex = snapshot?.executionIndex;
        const previousExecutionIndex = this.selected?.executionIndex;
        this.executionAdvancing = executionIndex !== undefined && (snapshot?.running ? this.advancing
            : previousExecutionIndex !== undefined && executionIndex !== previousExecutionIndex);
        this.selected = snapshot;
        this.receivedAt = now;
    }
    isAnimating(now: number): boolean {
        return (this.advancing || this.executionAdvancing) && now - this.receivedAt < extrapolationLimitMs;
    }
    position(now: number): bigint | null {
        return this.displayPosition(this.selected?.position, this.advancing, now);
    }
    executionIndex(now: number): bigint | null {
        return this.displayPosition(this.selected?.executionIndex, this.executionAdvancing, now);
    }
    private displayPosition(position: string | undefined, advancing: boolean, now: number): bigint | null {
        const state = this.selected;
        if (!state || position === undefined) return null;
        const elapsed = Math.max(0, Math.min(extrapolationLimitMs, now - this.receivedAt));
        const advance = advancing ? elapsed * state.sampleRate / 1000 : 0;
        return BigInt.asIntN(64, BigInt(position) + BigInt(Math.floor(advance)));
    }
}

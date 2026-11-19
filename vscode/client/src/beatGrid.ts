import { automationSampleIndex, automationSampleOffset } from "./automationCurveGeometry";

const bpmScale = 1_000_000;
const maximumBeatSubdivisions = 1024;
const uint32Max = 0xffffffff;
export const maximumBeatsPerBar = uint32Max;
const timelineExtent = 1n << 64n;
const timelineBegin = -(timelineExtent / 2n);

export const defaultBeatBpm = 120;
export const minimumBeatBpm = 1 / bpmScale;
export type BeatGrid = {
    scaledBpm: bigint;
    beatsPerBar: number;
    subdivisions: readonly number[];
};

export function beatSubdivisionCount(subdivisions: readonly unknown[]): number | null {
    let product = 1;
    for (const factor of subdivisions) {
        if (typeof factor !== "number" || !Number.isInteger(factor) || factor < 2) return null;
        product *= factor;
        if (product > maximumBeatSubdivisions) return null;
    }
    return product;
}

export function beatBpm(grid: BeatGrid): number { return Number(grid.scaledBpm) / bpmScale; }

export function maximumBeatBpm(sampleRate?: number | null, divisions = 1): number {
    return (sampleRate ? Math.min(uint32Max, Math.floor(sampleRate * 60 * bpmScale / divisions)) : uint32Max) / bpmScale;
}

export function maximumBeatSubdivisionCount(grid: BeatGrid, sampleRate?: number | null): number {
    if (!sampleRate) return maximumBeatSubdivisions;
    const capacity = BigInt(sampleRate) * 60n * BigInt(bpmScale) / grid.scaledBpm;
    return Math.min(maximumBeatSubdivisions, Number(capacity));
}

export function readBeatGrid(args: readonly unknown[]): BeatGrid | null {
    if (args.length > 3) return null;
    const [bpm = defaultBeatBpm, beatsPerBar = 4, subdivisions = [2, 2]] = args;
    const scaledBpm = typeof bpm === "number" ? Math.round(Number(bpm.toFixed(6)) * bpmScale) : NaN;
    if (!Number.isInteger(scaledBpm) || scaledBpm < 1 || scaledBpm > uint32Max
        || typeof beatsPerBar !== "number" || !Number.isInteger(beatsPerBar) || beatsPerBar < 1 || beatsPerBar > maximumBeatsPerBar
        || !Array.isArray(subdivisions) || beatSubdivisionCount(subdivisions) === null) return null;
    return { scaledBpm: BigInt(scaledBpm), beatsPerBar, subdivisions };
}

export function sameBeatGrid(left: BeatGrid, right: BeatGrid): boolean {
    return left.scaledBpm === right.scaledBpm && left.beatsPerBar === right.beatsPerBar
        && left.subdivisions.length === right.subdivisions.length
        && left.subdivisions.every((factor, index) => factor === right.subdivisions[index]);
}

function floorDivide(value: bigint, divisor: bigint): bigint {
    return value / divisor - (value % divisor < 0n ? 1n : 0n);
}

export type BeatGridTick = { x: number; depth: number; sample: bigint; bar?: bigint };

// Preview Beats::tock_coverage using the accepted constructor values. Keep
// calculation in sample indexes so independently drawn windows share marks.
export function beatGridTicks(grid: BeatGrid, window: { beginSeconds: number; spanSeconds: number },
    sampleRate: number, width: number): BeatGridTick[] {
    const { beginSeconds, spanSeconds } = window;
    if (!(spanSeconds > 0 && width >= 1) || !Number.isFinite(width)) return [];
    const begin = automationSampleIndex(beginSeconds, sampleRate);
    const end = automationSampleIndex(beginSeconds, sampleRate, spanSeconds);
    if (begin === undefined || end === undefined || end - begin >= timelineExtent) return [];

    // Depth zero marks bars, depth one marks beats, and each following
    // stride divides the preceding interval by the configured factor.
    const strides = [1n];
    for (const factor of [...grid.subdivisions].reverse()) strides.unshift(strides[0] * BigInt(factor));
    const beatStride = strides[0];
    const barStride = beatStride * BigInt(grid.beatsPerBar);
    strides.unshift(barStride);
    const numerator = BigInt(sampleRate) * 60n * BigInt(bpmScale);
    const denominator = grid.scaledBpm * beatStride;
    if (numerator < denominator) return [];

    const spacing = (stride: bigint) => Number(stride * numerator / denominator) / sampleRate / spanSeconds * width;
    const minimumSpacing = Math.max(8, width / 800);
    let step = barStride;
    for (const stride of strides) {
        if (spacing(stride) < minimumSpacing) break;
        step = stride;
    }
    // Thin dense bars by powers of two, retaining the bar at sample zero.
    while (spacing(step) < minimumSpacing) step *= 2n;

    const rounding = denominator / 2n;
    const markAtOrAfter = (sample: bigint) => -floorDivide(rounding - sample * denominator, step * numerator);
    const ticks: BeatGridTick[] = [];
    // A view spanning the signed seam sees opposite timeline ends next to
    // each other. Match the producer's mark phase independently on each side.
    const firstCycle = floorDivide(begin - timelineBegin, timelineExtent);
    const lastCycle = floorDivide(end - timelineBegin, timelineExtent);
    for (let cycle = firstCycle; cycle <= lastCycle; cycle++) {
        const offset = cycle * timelineExtent;
        const first = cycle === firstCycle ? begin - offset : timelineBegin;
        const last = cycle === lastCycle ? end - offset : timelineBegin + timelineExtent - 1n;
        const limit = markAtOrAfter(last + 1n);
        for (let tick = markAtOrAfter(first); tick < limit; tick++) {
            const mark = tick * step;
            const sample = floorDivide(mark * numerator + rounding, denominator);
            const x = automationSampleOffset(sample + offset, beginSeconds, sampleRate) / spanSeconds * width;
            if (x < 0 || x >= width) continue;
            const depth = strides.findIndex((stride) => mark % stride === 0n);
            ticks.push({ x, depth, sample, bar: depth === 0 ? mark / barStride + 1n : undefined });
        }
    }
    return ticks;
}

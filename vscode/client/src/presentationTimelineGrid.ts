import { gridGuideStrengths } from "./gridGuideStrength";
import type { PresentationMark, PresentationTimelineGrid } from "./presentationPlugin";
import { firstSignedSample, timelineSampleCount, timelineSamples, type PresentationTime, type TimelineSamples } from "./timelineSamples";
import type { SeekBeatWindow } from "./timelineGeometry";

function floorDivide(value: bigint, divisor: bigint): bigint {
    return value / divisor - (value % divisor < 0n ? 1n : 0n);
}

export function selectVisibleGridMarks<Mark extends { x: number; depth: number }>(
    candidates: readonly Mark[], width: number, spacing = 8,
): Mark[] {
    const displayed = new Map<number, Mark>();
    for (const mark of [...candidates].sort((a, b) => a.depth - b.depth || a.x - b.x)) {
        const slot = Math.floor(mark.x / spacing);
        let overlaps = false;
        for (let neighbor = slot - 1; neighbor <= slot + 1; ++neighbor) {
            const accepted = displayed.get(neighbor);
            if (accepted && Math.abs(accepted.x - mark.x) < spacing) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) displayed.set(slot, mark);
    }
    // A near-edge mark can suppress another mark without becoming a visible
    // snap target itself.
    return [...displayed.values()].filter(({ x }) => x >= 0 && x < width).sort((a, b) => a.x - b.x);
}

type Grid = { numerator: bigint; denominator: bigint; strides: readonly bigint[]; minimumSpacing: number };
type GridTick = { x: number; depth: number; sample: bigint; bar?: bigint };

export function regularTimelineGridTicks(
    { numerator, denominator, strides, minimumSpacing }: Grid, { origin, span }: TimelineSamples, width: number,
): GridTick[] {
    if (!(span > 0 && span <= Number(timelineSampleCount) && width >= 1)
        || !Number.isFinite(width) || denominator <= 0n || numerator < denominator) return [];
    const spacingLimit = Math.max(minimumSpacing, width / 800);
    const margin = span * Math.min(.25, spacingLimit / width);
    const begin = origin.index + BigInt(Math.floor(origin.fraction - margin));
    const end = origin.index + BigInt(Math.ceil(origin.fraction + span + margin));
    const majorStride = strides[0];
    const spacing = (stride: bigint) => Number(stride * numerator / denominator) / span * width;
    let step = majorStride;
    if (spacing(step) < spacingLimit) {
        while (spacing(step) < spacingLimit) step *= 2n;
    } else {
        for (const stride of strides) {
            if (spacing(stride) < spacingLimit / 2) break;
            step = stride;
        }
    }

    const rounding = denominator / 2n;
    const markAtOrAfter = (sample: bigint) => -floorDivide(rounding - sample * denominator, step * numerator);
    const firstCycle = floorDivide(begin - firstSignedSample, timelineSampleCount);
    const lastCycle = floorDivide(end - firstSignedSample, timelineSampleCount);
    const ticks: GridTick[] = [];
    for (let cycle = firstCycle; cycle <= lastCycle; cycle++) {
        const offset = cycle * timelineSampleCount;
        const first = cycle === firstCycle ? begin - offset : firstSignedSample;
        const last = cycle === lastCycle ? end - offset : firstSignedSample + timelineSampleCount - 1n;
        const limit = markAtOrAfter(last + 1n);
        for (let tick = markAtOrAfter(first); tick < limit; tick++) {
            const mark = tick * step;
            const sample = floorDivide(mark * numerator + rounding, denominator);
            const x = (Number(sample + offset - origin.index) - origin.fraction) / span * width;
            if (x < -spacingLimit || x > width + spacingLimit) continue;
            const depth = strides.findIndex((stride) => mark % stride === 0n);
            ticks.push({ x, depth, sample, bar: depth === 0 ? mark / majorStride + 1n : undefined });
        }
    }
    return selectVisibleGridMarks(ticks, width, spacingLimit);
}

function gridPeriod(grid: PresentationTimelineGrid): Grid {
    return { numerator: BigInt(grid.numerator), denominator: BigInt(grid.denominator),
        strides: grid.strides.map(BigInt), minimumSpacing: grid.minimumSpacing };
}

export function renderPresentationTimelineGrid(grid: PresentationTimelineGrid, time: PresentationTime,
    width: number, height: number, labels: boolean,
): { marks: PresentationMark[]; samples: bigint[] } {
    const ticks = regularTimelineGridTicks(gridPeriod(grid), timelineSamples(time), width);
    const strengths = gridGuideStrengths(ticks, ({ x }) => x);
    const marks: PresentationMark[] = [];
    let nextLabelX = 0;
    for (const { sample, depth, x, bar } of ticks) {
        marks.push({ id: `grid:${sample}`, type: "polyline", ink: grid.ink, opacity: (strengths.get(depth) ?? 1) * grid.opacity,
            points: [{ x, y: height * grid.lineStarts[depth] }, { x, y: height }] });
        if (grid.labelMajor && labels && bar !== undefined && x >= nextLabelX && x <= width - 36) {
            const text = String(bar);
            marks.push({ id: `grid-label:${sample}`, type: "text", ink: "muted", x: x + 4, y: height - 4, text });
            nextLabelX = x + Math.max(60, 25 + text.length * 7);
        }
    }
    return { marks, samples: ticks.map(({ sample }) => sample) };
}

export function openPresentationTimelineReader(grid: PresentationTimelineGrid) {
    const period = gridPeriod(grid);
    let cached: { first: bigint; length: bigint; width: number; samples: bigint[] } | undefined;
    return { marks({ first, length }: SeekBeatWindow, _sampleRate: number, width: number) {
        if (cached?.first !== first || cached.length !== length || cached.width !== width) {
            const ticks = regularTimelineGridTicks(period, { origin: { index: first, fraction: 0 }, span: Number(length) }, width);
            cached = { first, length, width, samples: ticks.map(({ sample }) => sample) };
        }
        return cached.samples;
    } };
}

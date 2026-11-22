import type { ObservationTimeSpan } from "./observations";

export const timelineSampleCount = 1n << 64n;
export const firstSignedSample = -(1n << 63n);
export const projectSampleRateFallback = 48000;

export type SamplePosition = { index: bigint; fraction: number };
export type TimelineSamples = { origin: SamplePosition; span: number };
// Decimal sample indices keep the same precision in a presentation worker.
export type PresentationTime = { beginSample: string; beginFraction: number; spanSamples: number;
    sampleRate?: number; gridMode?: "seconds" | "beats" | "custom" };

export function shiftSamplePosition(origin: SamplePosition, offset: number): SamplePosition {
    // Split before adding so a wide offset does not absorb the origin's fraction.
    const whole = Math.floor(offset);
    const fraction = origin.fraction + (offset - whole);
    const carry = Math.floor(fraction);
    return { index: BigInt.asIntN(64, origin.index + BigInt(whole) + BigInt(carry)), fraction: fraction - carry };
}

export function timelineSamples(time: PresentationTime): TimelineSamples {
    return { origin: { index: BigInt(time.beginSample), fraction: time.beginFraction }, span: time.spanSamples };
}

export function timelineSampleAt(view: TimelineSamples, fraction: number): bigint {
    const position = shiftSamplePosition(view.origin, view.span * fraction);
    return BigInt.asIntN(64, position.index + (position.fraction >= .5 ? 1n : 0n));
}

// Place an offscreen sample next to the nearer edge of the view. Inside the
// view, use forward distance, even when it exceeds half the sample circle.
export function timelineSampleOffset(view: TimelineSamples, sample: bigint): number {
    let offset = BigInt.asUintN(64, sample - view.origin.index);
    if (offset === 0n) return view.origin.fraction && view.span === Number(timelineSampleCount)
        ? view.span - view.origin.fraction : -view.origin.fraction;
    const cut = (Number(timelineSampleCount) + view.span) / 2;
    if (Number(offset) >= cut && view.span < Number(timelineSampleCount)) offset -= timelineSampleCount;
    return Number(offset) - view.origin.fraction;
}

export function timelineSampleExtent(view: TimelineSamples): bigint {
    const whole = Math.floor(view.span);
    return BigInt(whole) + BigInt(Math.ceil(view.span - whole + view.origin.fraction));
}

export function timelineObservationSpan(view: TimelineSamples): ObservationTimeSpan {
    const extent = timelineSampleExtent(view);
    // A partial view includes the sample at or immediately before its left
    // edge for interpolation. For a full lap that sample belongs at the right.
    return extent > timelineSampleCount ? { begin: String(BigInt.asIntN(64, view.origin.index + 1n)),
        length: String(timelineSampleCount) } : { begin: String(view.origin.index), length: String(extent) };
}

export function timelineObservationOffset(view: TimelineSamples): number {
    return (timelineSampleExtent(view) > timelineSampleCount ? 1 : 0) - view.origin.fraction;
}

/** Discrete events need only the samples inside the viewport's half-open interval. */
export function timelineVisibleEventSpan(view: TimelineSamples): ObservationTimeSpan {
    const beginOffset = view.origin.fraction > 0 ? 1n : 0n;
    return { begin: String(BigInt.asIntN(64, view.origin.index + beginOffset)),
        length: String(timelineSampleExtent(view) - beginOffset) };
}

export function signedTimelineSpans(begin: bigint, length: bigint): Array<{ begin: bigint; length: bigint; offset: bigint }> {
    const first = BigInt.asIntN(64, begin);
    const firstLength = -firstSignedSample - first;
    return length <= firstLength ? [{ begin: first, length, offset: 0n }]
        : [{ begin: first, length: firstLength, offset: 0n },
            { begin: firstSignedSample, length: length - firstLength, offset: firstLength }];
}

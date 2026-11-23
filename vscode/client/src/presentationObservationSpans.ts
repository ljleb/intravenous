import type { ObservationTimeSpan } from "./observations";
import type { ObservationSpan } from "./observationWindows";
import type { PresentationObservation } from "./presentationPlugin";
import { timelineSampleCount } from "./timelineSamples";

export function presentationObservationSpans(observation: PresentationObservation, visible: ObservationTimeSpan): ObservationSpan[] {
    const shift = BigInt.asUintN(64, BigInt(observation.beginSample) - BigInt(visible.begin));
    const extent = BigInt(visible.length);
    const spans: ObservationSpan[] = [];
    for (const span of observation.spans) {
        const begin = BigInt(span.begin), end = BigInt(span.end), windowOffset = BigInt(span.windowOffset);
        for (const offset of [shift - timelineSampleCount, shift]) {
            const start = begin + offset, stop = end + offset;
            const clippedBegin = start < 0n ? 0n : start;
            const clippedEnd = stop > extent ? extent : stop;
            if (clippedBegin < clippedEnd)
                spans.push({ begin: clippedBegin, end: clippedEnd, windowOffset: windowOffset + offset, result: span.result });
        }
    }
    return spans;
}

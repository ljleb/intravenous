// @doc docs/api/explanation/presentation_execution.md
import type { PresentationObservation, PresentationSnapshot } from "./presentationPlugin";
import { timelineSamples } from "./timelineSamples";

export const triggerColumnLimit = 1024;
export type TriggerColumns = {
    id: string; columns: Uint32Array; count: bigint;
    partial: boolean; status: PresentationObservation["status"]; error?: string;
};

// Retained windows can have a different scale or origin from the current
// viewport. Intersect their bins with the host's selected, disjoint spans
// before converting any sample index to a floating-point position.
export function presentationTriggerColumns({ observations, viewport: { time, width } }: PresentationSnapshot): TriggerColumns[] {
    if (!time?.sampleRate) return [];
    const view = timelineSamples(time);
    const visibleOffset = view.origin.fraction > 0 ? 1 - view.origin.fraction : 0;
    const pixelsPerSample = width / view.span;
    return Object.entries(observations).flatMap(([id, observation]) => {
        if (!observation) return [];
        const columns = new Set<number>();
        let count = 0n;
        let partial = false;
        for (const span of observation.spans) {
            if (span.result.status !== "ready") continue;
            const begin = BigInt(span.begin), end = BigInt(span.end);
            const windowBegin = BigInt(span.result.window.begin);
            const base = BigInt(span.windowOffset);
            for (const bin of span.result.triggerBins ?? []) {
                const binBegin = base + BigInt.asUintN(64, BigInt(bin.begin) - windowBegin);
                const binEnd = binBegin + BigInt(bin.length);
                if (binEnd <= begin || binBegin >= end) continue;
                const complete = binBegin >= begin && binEnd <= end;
                partial ||= !complete;
                if (complete) count += BigInt(bin.count);
                const offset = binBegin + BigInt.asUintN(64, BigInt(bin.first) - BigInt(bin.begin));
                if (offset < begin || offset >= end) continue;
                const x = (Number(offset) + visibleOffset) * pixelsPerSample;
                if (x < 0 || x > width) continue;
                if (!complete) ++count;
                columns.add(Math.min(Math.floor(x), Math.max(0, Math.ceil(width) - 1)));
            }
        }
        const stride = Math.ceil(width / triggerColumnLimit);
        const visible = columns.size <= triggerColumnLimit ? columns
            : new Set(Array.from(columns, (column) => Math.floor(column / stride) * stride));
        const status = observation.result?.status === "ready" ? "ready" : observation.status;
        return [{ id, columns: Uint32Array.from(visible), count, partial, status, error: observation.error }];
    });
}

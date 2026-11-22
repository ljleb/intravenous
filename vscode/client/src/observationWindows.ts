import { compareObservationWindowVersions, sameObservationWindow, type ObservationTimeSpan,
    type ObservationWindow, type ObservationWindowResult } from "./observations";
import { timelineSampleCount } from "./timelineSamples";

export type ObservationTier = "visible" | "prefetch";
export type ObservationWindowRequest = { slot: string; window: ObservationWindow; prefetch: boolean; prefetchRank?: number };
const previewOffsets = [0, 1, -1, 2, -2, 3];
const nearbyOffsetCount = 2;
const maxWindowLength = (1n << 63n) - 1n;
export const observationPrefetchMarginPx = 240;

export function observationViewportWindow(visible: ObservationWindow, indexedEvents = false): ObservationWindow {
    if (!indexedEvents) return visible;
    const first = BigInt(visible.begin), length = BigInt(visible.length);
    if (length <= 0n || length >= (1n << 62n)) return visible;
    // Align the request and keep a margin on both sides. This is safe for
    // indexed sources, whose query budget does not require reading every event.
    let step = 1n << BigInt(Math.max(0, Math.ceil(Math.log2(Number(length) / 4))));
    if (4n * step < length) step *= 2n;
    const begin = first - (first % step + step) % step - step;
    return { ...visible, begin: String(BigInt.asIntN(64, begin)), length: String(7n * step) };
}

export function observationWindowId(id: string, slot: string): string {
    return slot === "visible" ? id : `${id}:${slot}`;
}

export function observationWindowIds(id: string): string[] {
    // A viewport covers at most one circle. Three requests suffice even
    // when no individual request may cover half of it.
    return [id, observationWindowId(id, "visible:1"), observationWindowId(id, "visible:2"),
        ...previewOffsets.map((_, index) => observationWindowId(id, `tile:${index}`))];
}

export function predictedObservationRows(scrollTop: number, viewportHeight: number, headerHeight: number,
    laneHeight: number, laneCount: number, scrollSpeed: number) {
    if (viewportHeight <= headerHeight || laneHeight <= 0 || !laneCount) return;
    const maximumScroll = Math.max(0, headerHeight + laneCount * laneHeight - viewportHeight);
    const direction = scrollSpeed < 0 ? -1 : 1;
    const lead = Math.max(laneHeight, Math.min(observationPrefetchMarginPx, Math.abs(scrollSpeed) * 90));
    const target = Math.max(0, Math.min(maximumScroll, scrollTop + direction * lead));
    if (target === scrollTop) return;
    const begin = Math.max(0, Math.floor((target - headerHeight) / laneHeight));
    const end = Math.min(laneCount, Math.ceil((target + viewportHeight - headerHeight) / laneHeight));
    return { begin, end };
}

export function visibleObservationRequests(visible: ObservationWindow): ObservationWindowRequest[] {
    const span = BigInt(visible.length);
    if (span <= 0n || span > timelineSampleCount) return [];
    const requests: ObservationWindowRequest[] = [];
    const parts = (span + maxWindowLength - 1n) / maxWindowLength;
    const origin = BigInt(visible.begin);
    const budget = visible.points ? BigInt(Math.max(Number(parts), visible.points)) : 0n;
    for (let part = 0n; part < parts; ++part) {
        const offset = span * part / parts, end = span * (part + 1n) / parts;
        const points = Number(budget * (part + 1n) / parts - budget * part / parts);
        const window = parts === 1n ? visible : { ...visible, begin: String(BigInt.asIntN(64, origin + offset)),
            length: String(end - offset), points };
        requests.push({ slot: part === 0n ? "visible" : `visible:${part}`, window, prefetch: false });
    }
    return requests;
}

export function observationWindowRequests(visible: ObservationWindow, tier: ObservationTier): ObservationWindowRequest[] {
    const requests = tier === "visible" ? visibleObservationRequests(visible) : [];
    const span = BigInt(visible.length);
    if (!visible.points) return requests;
    if (span <= 0n || span >= 1n << 62n) return requests;
    let length = 1n << BigInt(Math.max(0, Math.ceil(Math.log2(Number(span)))));
    if (length < span) length *= 2n;
    const begin = BigInt(visible.begin);
    const firstTile = (begin - (begin % length + length) % length) / length;
    const slots = BigInt(previewOffsets.length);
    for (const offset of tier === "visible" ? previewOffsets : previewOffsets.slice(0, nearbyOffsetCount)) {
        const tile = firstTile + BigInt(offset);
        const far = offset === -2 || offset === 3;
        const points = Math.max(2, Math.ceil(Math.min(visible.points, 512) * (far ? 8 : 28) / 512));
        // Rotate IDs as the aligned tiles move. The same tiles survive both
        // a small pan and promotion of a nearby lane into the viewport.
        const slot = `tile:${(tile % slots + slots) % slots}`;
        requests.push({ slot, window: { ...visible, begin: String(BigInt.asIntN(64, tile * length)),
            length: String(length), points }, prefetch: true, prefetchRank: far ? 2 : offset === 0 || offset === 1 ? 0 : 1 });
    }
    return requests;
}

export type ObservationSpan = { result: ObservationWindowResult; begin: bigint; end: bigint; windowOffset: bigint };

export function observationSpans(results: readonly ObservationWindowResult[], visible: ObservationTimeSpan, fine: readonly ObservationWindow[] = []) {
    const origin = BigInt(visible.begin), span = BigInt(visible.length);
    const windows = results.flatMap((result) => {
        if (result.status !== "ready" && result.status !== "unavailable") return [];
        const forward = BigInt.asUintN(64, BigInt(result.window.begin) - origin);
        const length = BigInt(result.window.length);
        return [forward - timelineSampleCount, forward].flatMap((windowOffset) => {
            const finish = windowOffset + length;
            const begin = windowOffset > 0n ? windowOffset : 0n, end = finish < span ? finish : span;
            return begin < end ? [{ result, begin, end, windowOffset,
                fine: fine.some((window) => sameObservationWindow(result.window, window)),
                density: result.window.points / Number(length) }] : [];
        });
    }).sort((a, b) => Number(b.fine) - Number(a.fine) || b.density - a.density
        || compareObservationWindowVersions(b.result, a.result));
    const edges = [...new Set(windows.flatMap(({ begin, end }) => [begin, end]))].sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
    const spans: ObservationSpan[] = [];
    for (let i = 1; i < edges.length; ++i) {
        const begin = edges[i - 1], end = edges[i];
        const selected = windows.find((window) => window.begin <= begin && end <= window.end);
        if (!selected) continue;
        const last = spans.at(-1);
        if (last?.result === selected.result && last.windowOffset === selected.windowOffset && last.end === begin) last.end = end;
        else spans.push({ result: selected.result, begin, end, windowOffset: selected.windowOffset });
    }
    return spans;
}

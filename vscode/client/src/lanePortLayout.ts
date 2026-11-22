export const lanePortRowGap = 3;
export const lanePortFoldedSocketHeight = 8;
export const compactLanePortRowHeight = 24;
export const lanePortColumns = { gap: 4, handleWidth: 24, polarityWidth: 12, polarityInset: 2 };

const fullLanePortRowHeight = 31;
const compactPortControls = { knobSize: 20, handleWidth: 14 };

export function lanePortRowHeight(availableHeight: number): number {
    const rowSpace = availableHeight - lanePortFoldedSocketHeight;
    if (rowSpace >= fullLanePortRowHeight) return fullLanePortRowHeight;
    if (rowSpace >= compactLanePortRowHeight) return compactLanePortRowHeight;
    return compactPortControls.knobSize;
}

export function lanePortControls(width: number | undefined, knobSize: number):
    { knobSize: number; handleWidth: number } | null {
    const { gap, handleWidth, polarityWidth, polarityInset } = lanePortColumns;
    const fits = (size: number, handle: number) => width === undefined
        || width >= 2 * (size + polarityWidth - polarityInset + handle) + gap;
    // Reserve both columns so one-sided lanes and event ports follow the same geometry.
    if (fits(knobSize, handleWidth)) return { knobSize, handleWidth };
    return fits(compactPortControls.knobSize, compactPortControls.handleWidth) ? compactPortControls : null;
}

const minimumContinuationColumnWidth = 144;
const continuationViewportInset = 4;

export function lanePortContinuationColumn(column: { left: number; width: number }, viewportWidth: number,
    direction: "input" | "output") {
    const maxWidth = Math.max(0, viewportWidth - 2 * continuationViewportInset);
    const width = Math.min(maxWidth, Math.max(column.width, minimumContinuationColumnWidth));
    const anchoredLeft = direction === "output" ? column.left + column.width - width : column.left;
    const left = Math.max(continuationViewportInset, Math.min(anchoredLeft, viewportWidth - continuationViewportInset - width));
    return { left, width };
}

// Fit complete rows in definition order. Leave space for the folded socket
// only when the column has more rows than it can show.
export function lanePortLayout<T>(ports: readonly T[], height: number,
    rowHeight = 22, foldedSocketHeight = 0): {
    direct: readonly T[]; folded: readonly T[]; continuationOffset: number;
} {
    const available = Number.isFinite(height) ? Math.max(0, height) : 0;
    let directCount = 0;
    let countWithSocket = 0;
    let usedWithSocket = 0;
    let used = 0;
    while (directCount < ports.length) {
        const next = used + (directCount ? lanePortRowGap : 0) + rowHeight;
        if (next > available) break;
        used = next;
        ++directCount;
        if (next + foldedSocketHeight <= available) {
            countWithSocket = directCount;
            usedWithSocket = next;
        }
    }
    if (directCount < ports.length) {
        directCount = countWithSocket;
        used = usedWithSocket;
    }
    return { direct: ports.slice(0, directCount), folded: ports.slice(directCount),
        continuationOffset: directCount ? used + lanePortRowGap : 0 };
}

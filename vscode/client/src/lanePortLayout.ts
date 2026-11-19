export const lanePortRowGap = 3;

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

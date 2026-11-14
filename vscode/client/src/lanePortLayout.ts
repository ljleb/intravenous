// Each direction uses its own vertical column. Reserve one handle for the
// remainder only when the whole list cannot fit. Never reorder the ports.
export function lanePortLayout<T>(ports: readonly T[], height: number,
    portHeight: (port: T) => number = () => 22, gap = 3): {
    direct: readonly T[]; folded: readonly T[];
} {
    const available = Number.isFinite(height) ? Math.max(0, height) : 0;
    const heights = ports.map(portHeight);
    const complete = heights.reduce((sum, value) => sum + value, 0) + Math.max(0, ports.length - 1) * gap;
    if (complete <= available || ports.length <= 1 && complete <= 22) return { direct: ports.slice(), folded: [] };
    let directCount = 0;
    let used = 0;
    for (const rowHeight of heights) {
        const next = used + (directCount ? gap : 0) + rowHeight;
        if (next + gap + 22 > available) break; // Reserve the overflow handle.
        used = next;
        ++directCount;
    }
    return { direct: ports.slice(0, directCount), folded: ports.slice(directCount) };
}

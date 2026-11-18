// Fit complete rows in definition order. The last direct row anchors any
// continuation. A column with no room for a complete row uses a folded handle.
export function lanePortLayout<T>(ports: readonly T[], height: number,
    portHeight: (port: T) => number = () => 22, gap = 3): {
    direct: readonly T[]; folded: readonly T[];
} {
    const available = Number.isFinite(height) ? Math.max(0, height) : 0;
    let directCount = 0;
    let used = 0;
    for (const port of ports) {
        const next = used + (directCount ? gap : 0) + portHeight(port);
        if (next > available) break;
        used = next;
        ++directCount;
    }
    return { direct: ports.slice(0, directCount), folded: ports.slice(directCount) };
}

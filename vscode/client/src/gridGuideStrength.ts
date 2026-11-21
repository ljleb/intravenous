// Strength is normalized so the ruler and lanes can use different maximum opacities.
export function gridGuideStrengths<T extends { depth: number }>(
    guides: readonly T[], position: (guide: T) => number,
): ReadonlyMap<number, number> {
    const gaps = new Map<number, number>();
    const preceding: Array<{ depth: number; pixel: number }> = [];
    const sorted = guides.map((guide) => ({ depth: guide.depth, pixel: position(guide) }))
        .sort((a, b) => a.pixel - b.pixel);
    const recordGap = (depth: number, gap: number) => {
        gaps.set(depth, Math.min(gaps.get(depth) ?? Infinity, gap));
    };
    // Each pair contributes to the first depth where both guides are visible.
    // The stack finds the nearest same-or-coarser guide in either direction.
    for (const guide of sorted) {
        const { depth, pixel } = guide;
        recordGap(depth, Infinity);
        while (preceding.length && preceding[preceding.length - 1].depth > depth) {
            const finer = preceding.pop()!;
            recordGap(finer.depth, pixel - finer.pixel);
        }
        const neighbor = preceding[preceding.length - 1];
        if (neighbor) recordGap(depth, pixel - neighbor.pixel);
        preceding.push(guide);
    }

    // Squared spacing rolls off contrast smoothly as lines crowd together.
    // The overall gap limits total brightness, even with only one visible depth.
    const strength = (gap: number) => 1 / (1 + (24 / gap) ** 2);
    const overallGap = Math.min(...gaps.values());
    const brightness = .6 + .4 * strength(overallGap);
    let gap = Infinity;
    return new Map([...gaps.keys()].sort((a, b) => a - b).map((depth, level) => {
        gap = Math.min(gap, gaps.get(depth)!);
        return [depth, brightness * strength(gap) * .82 ** level];
    }));
}

// The server owns filtering while the client owns local lane ordering.  Before
// the first result arrives, a restored webview has no meaningful total count;
// request a modest page instead of treating its single placeholder row as the
// complete server window.
export function requestedLaneViewCount(
    visibleLaneCount: number,
    totalLaneCount: number,
): number {
    return Math.max(visibleLaneCount, totalLaneCount, 24);
}

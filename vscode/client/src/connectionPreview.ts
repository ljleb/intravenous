import type { Point } from "./graphScope";

export type PortColorColumns = {
    nodeKey: string;
    inputX: number;
    outputX: number;
    top: number;
    bottom: number;
};

// Zero is input yellow; one is output blue. Only the cursor coordinates and
// measured column positions determine the blend, including over popovers.
export function connectionCursorOutputFraction(point: Point, columns: readonly PortColorColumns[]): number {
    let nearest: PortColorColumns | undefined;
    let distance = Infinity;
    for (const row of columns) {
        const verticalDistance = Math.max(row.top - point.y, point.y - row.bottom, 0);
        if (verticalDistance < distance) { distance = verticalDistance; nearest = row; }
    }
    if (!nearest || nearest.outputX === nearest.inputX) return 0.5;
    return Math.max(0, Math.min(1, (point.x - nearest.inputX) / (nearest.outputX - nearest.inputX)));
}

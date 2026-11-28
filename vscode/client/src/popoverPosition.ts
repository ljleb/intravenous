import type { Point } from "./graphScope";

type Size = { width: number; height: number };

export function fitPopoverAtPoint(point: Point, size: Size, viewport: Size, inset = 4): Point {
    return {
        x: Math.max(inset, Math.min(point.x, viewport.width - size.width - inset)),
        y: Math.max(inset, Math.min(point.y, viewport.height - size.height - inset)),
    };
}

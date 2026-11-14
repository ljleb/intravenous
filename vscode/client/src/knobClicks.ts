import type { Point } from "./graphScope";

type Press = { key: string; point: Point; time: number; travel: number };
// Pointer lock retargets click/dblclick to the editor, so recognize two short
// presses by semantic knob identity. A drag is never the first half of a reset.
export class KnobClicks {
    private active: Press | null = null;
    private previous: Press | null = null;
    press(key: string, point: Point, time: number, clickCount = 0): boolean {
        const previous = this.previous;
        this.previous = null;
        if (previous?.key === key && (time - previous.time <= 500 || clickCount === 2)
            && Math.hypot(point.x - previous.point.x, point.y - previous.point.y) <= 4) {
            this.active = null;
            return true;
        }
        this.active = { key, point, time, travel: 0 };
        return false;
    }
    move(dx: number, dy: number): void {
        if (this.active) this.active.travel += Math.hypot(dx, dy);
    }
    release(time: number): void {
        const active = this.active;
        this.active = null;
        this.previous = active && active.travel <= 4 && time - active.time <= 500
            ? { ...active, time } : null;
    }
    cancel(): void { this.active = null; this.previous = null; }
}

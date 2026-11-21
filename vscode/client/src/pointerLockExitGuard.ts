import type { Point } from "./graphScope";

// After a knob drag releases pointer lock, the host (observed in VSCodium's
// Chromium under fractional scaling) reports a position displaced in the
// drag's direction, while the visible system cursor stays where the drag began.
// The displaced position first arrives as a move with zero movement; presses and
// boundary events then use it until a real movement re-synchronizes the
// position. Until then the only trustworthy position is the lock origin.
export type GhostEventAction =
    // Not affected: let the event through unchanged.
    | "pass"
    // A position change the user did not make: deliver it to nobody.
    | "drop"
    // A user action at the displaced position: deliver it at the origin.
    | "retarget"
    // The first real movement: the position is trustworthy again. Deliver it;
    // the webview's TrustedHover then moves hover from the origin to it.
    | "settle";

const moves = new Set(["pointermove", "mousemove", "pointerrawupdate"]);
const boundaries = new Set([
    "pointerover", "pointerout", "pointerenter", "pointerleave",
    "mouseover", "mouseout", "mouseenter", "mouseleave",
]);
const actions = new Set([
    "pointerdown", "pointerup", "mousedown", "mouseup",
    "click", "dblclick", "auxclick", "contextmenu",
]);

export type GhostEvent = {
    type: string; movementX?: number; movementY?: number;
    position?: Point;
    // Set for pointer events; "mouse" is the only displaced input.
    pointerType?: string;
    // Set for compatibility mouse events that a touch produced.
    firesTouchEvents?: boolean;
};

export class PointerLockExitGuard {
    private lockOrigin: Point | null = null;
    private displacedPosition: Point | undefined;

    // A granted pointer lock ended for a drag that began at `origin`.
    begin(origin: Point): void {
        this.lockOrigin = { ...origin };
        this.displacedPosition = undefined;
    }
    // Focus moved elsewhere; positions from a later return are real.
    end(): void {
        this.lockOrigin = null;
        this.displacedPosition = undefined;
    }
    origin(): Point | null { return this.lockOrigin; }

    // Classifies one unlocked window event. Locked events are never passed
    // here: while locked, positions are frozen and movement drives the knob.
    classify(event: GhostEvent): GhostEventAction {
        if (!this.lockOrigin) return "pass";
        // Touch and pen report where they are, and do not end the mouse's guard.
        if ((event.pointerType && event.pointerType !== "mouse") || event.firesTouchEvents) return "pass";
        if (moves.has(event.type)) {
            const { position } = event;
            const displaced = this.displacedPosition;
            // Pointer and mouse events can describe the same restoration
            // move. Some hosts then continue reporting zero relative deltas
            // even as the absolute cursor position changes.
            if (event.movementX || event.movementY || (position && displaced
                && (Math.abs(position.x - displaced.x) >= 1 || Math.abs(position.y - displaced.y) >= 1))) {
                this.end();
                return "settle";
            }
            if (!displaced && position && (position.x !== this.lockOrigin.x || position.y !== this.lockOrigin.y))
                this.displacedPosition = position;
            return "drop";
        }
        if (boundaries.has(event.type)) return "drop";
        if (actions.has(event.type)) return "retarget";
        return "pass";
    }
}

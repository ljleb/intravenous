import type { Point } from "./graphScope";
import type { GhostEventAction, PointerLockExitGuard } from "./pointerLockExitGuard";

export type PointerLockExitEnvironment = {
    locked(): boolean;
    elementAt(point: Point): EventTarget | null;
    // A browser event of the same kind as `event`, built from `init`.
    copy(event: MouseEvent, init: PointerEventInit): Event;
    trace?(action: GhostEventAction, event: MouseEvent): void;
};

// The event's fields at another position. Pointer fields are kept so pointer
// capture and pointer lock still apply to the original pointer.
export function relocatedPointerInit(event: MouseEvent, at: Point): PointerEventInit {
    const init: PointerEventInit = {
        bubbles: event.bubbles, cancelable: event.cancelable, composed: event.composed,
        detail: event.detail, clientX: at.x, clientY: at.y,
        screenX: event.screenX + at.x - event.clientX, screenY: event.screenY + at.y - event.clientY,
        ctrlKey: event.ctrlKey, shiftKey: event.shiftKey, altKey: event.altKey, metaKey: event.metaKey,
        button: event.button, buttons: event.buttons, relatedTarget: null,
    };
    if ("pointerId" in event) {
        const pointer = event as PointerEvent;
        Object.assign(init, {
            pointerId: pointer.pointerId, pointerType: pointer.pointerType, isPrimary: pointer.isPrimary,
            width: pointer.width, height: pointer.height, pressure: pointer.pressure,
        });
    }
    return init;
}

// Applies PointerLockExitGuard to window capture-phase events: displaced moves
// and boundary events reach no listener, and presses, releases and clicks are
// re-dispatched at the lock origin, where the cursor visibly is.
export class PointerLockExitRedirect {
    private readonly replayed = new WeakSet<Event>();
    constructor(private readonly guard: PointerLockExitGuard, private readonly env: PointerLockExitEnvironment) {}

    handle(event: MouseEvent): void {
        if (this.replayed.has(event) || this.env.locked()) return;
        const origin = this.guard.origin();
        const action = this.guard.classify({
            type: event.type, movementX: event.movementX, movementY: event.movementY,
            pointerType: "pointerType" in event ? (event as PointerEvent).pointerType : undefined,
            firesTouchEvents: (event as { sourceCapabilities?: { firesTouchEvents?: boolean } | null })
                .sourceCapabilities?.firesTouchEvents,
        });
        if (action === "pass" || !origin) return;
        this.env.trace?.(action, event);
        if (action === "settle") return;
        event.stopImmediatePropagation();
        if (event.cancelable) event.preventDefault();
        if (action === "drop") return;
        const target = this.env.elementAt(origin);
        if (!target) return;
        const copy = this.env.copy(event, relocatedPointerInit(event, origin));
        this.replayed.add(copy);
        target.dispatchEvent(copy);
    }
}

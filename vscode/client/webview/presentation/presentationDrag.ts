import type { PresentationGestureInput, PresentationModifiers, PresentationPoint, PresentationTarget } from "../../src/presentationPlugin";
import type { StatusHint } from "../../src/statusBarText";
import type { ObjectDrag } from "../graph/context";
import type { CursorFollowTarget, CursorTrace } from "../graph/cursorReturn";

function modifiers(event: MouseEvent | KeyboardEvent): PresentationModifiers {
    return { shift: event.shiftKey, alt: event.altKey, control: event.ctrlKey, meta: event.metaKey };
}

export function presentationDrag(event: PointerEvent, bounds: Pick<DOMRect, "left" | "top">,
    target: Pick<PresentationTarget, "id" | "drag" | "wheel">, receive: (input: PresentationGestureInput) => boolean | void,
    options?: { cursorReturn?: () => CursorFollowTarget | undefined; hint?: () => StatusHint | undefined; hideCursor?: boolean;
        trace?: CursorTrace }): ObjectDrag {
    const local = ({ x, y }: PresentationPoint) => ({ x: x - bounds.left, y: y - bounds.top });
    let point = local({ x: event.clientX, y: event.clientY });
    let held = modifiers(event);
    const send = (phase: PresentationGestureInput["phase"], delta?: PresentationPoint, wheel?: number) =>
        receive({ type: "gesture", id: target.id, phase, point, delta, wheel, modifiers: held });
    const move = (next: PresentationPoint) => {
        const delta = { x: next.x - point.x, y: next.y - point.y };
        if (delta.x || delta.y) { point = next; send("move", delta); }
    };
    return {
        lock: target.drag === "absolute" ? false : undefined,
        trace: options?.trace,
        hideCursor: options?.hideCursor,
        begin: () => send("begin") !== false,
        hint: options?.hint,
        wheel: target.wheel ? (delta) => { send("wheel", undefined, delta); return true; } : undefined,
        move: (position) => move(local(position)),
        moveBy: (delta) => move({ x: point.x + delta.x, y: point.y + delta.y }),
        modifiers(event) {
            const next = modifiers(event);
            if (next.shift === held.shift && next.alt === held.alt && next.control === held.control && next.meta === held.meta) return;
            held = next;
            send("modifiers");
        },
        finish(release, lockOrigin) {
            if (release && !lockOrigin) move(local(release));
            send(release ? "finish" : "cancel");
            return release ? options?.cursorReturn?.() : undefined;
        },
    };
}

import type { Point } from "./graphScope";
import { hoverTransition } from "./hoverTransition";

// Hover in the webviews comes only from this unit. Components listen for these
// events instead of native pointerenter/pointerleave/mouseenter/mouseleave, and
// styles select [data-hovered] instead of :hover. The browser derives those
// native signals from its own pointer position, which is displaced after a knob's
// pointer lock ends until the pointer really moves (see PointerLockExitGuard).
// This unit hit-tests only positions from pointer moves it receives, so a root
// that discards untrusted moves before this unit's listener keeps hover correct.
export const hoverEnterEvent = "iv-hoverenter";
export const hoverLeaveEvent = "iv-hoverleave";
export const hoveredAttribute = "data-hovered";

export type HoverElement = {
    readonly parentElement: HoverElement | null;
    readonly isConnected: boolean;
    setAttribute(name: string, value: string): void;
    removeAttribute(name: string): void;
    dispatchEvent(event: Event): boolean;
};
export type HoverEnvironment = {
    window: EventTarget;
    document: EventTarget;
    locked(): boolean;
    elementAt(point: Point): HoverElement | null;
    viewport(): { width: number; height: number };
    // Calls `changed` after DOM changes that can move content under the pointer.
    observeChanges(changed: () => void): () => void;
    requestFrame(callback: () => void): number;
    cancelFrame(handle: number): void;
};

export function browserHoverEnvironment(): HoverEnvironment {
    return {
        window, document,
        locked: () => !!document.pointerLockElement,
        elementAt: (point) => document.elementFromPoint(point.x, point.y),
        viewport: () => ({ width: window.innerWidth, height: window.innerHeight }),
        observeChanges: (changed) => {
            const observer = new MutationObserver(changed);
            observer.observe(document.body, {
                childList: true, subtree: true, attributes: true,
                attributeFilter: ["class", "style", "hidden", "disabled", "data-status-hint"],
            });
            return () => observer.disconnect();
        },
        requestFrame: (callback) => requestAnimationFrame(callback),
        cancelFrame: (handle) => cancelAnimationFrame(handle),
    };
}

type PointerFields = { clientX: number; clientY: number; pointerId?: number; relatedTarget?: unknown };

export class TrustedHover {
    private chain: HoverElement[] = [];
    private position: Point | null = null;
    private frame = 0;
    // As in a browser, a captured pointer changes no hover until it is released.
    private readonly captured = new Set<number>();
    private readonly stopObserving: () => void;
    private readonly listeners: Array<[EventTarget, string, EventListener, boolean]> = [];

    constructor(private readonly env: HoverEnvironment = browserHoverEnvironment(),
        private readonly refreshed?: (target: HoverElement | null, pointerMoved: boolean) => void) {
        const pointer = (event: Event) => event as unknown as PointerFields;
        this.listen(env.window, "pointermove", (event) => this.moved(pointer(event)), true);
        this.listen(env.window, "pointerout", (event) => this.left(pointer(event)), true);
        // The pointer can come to rest over different content without moving.
        this.listen(env.window, "scroll", () => this.schedule(), true);
        this.listen(env.window, "resize", () => this.schedule(), false);
        this.listen(env.document, "pointerlockchange", () => this.schedule(), false);
        this.listen(env.window, "gotpointercapture", (event) => this.captured.add(pointer(event).pointerId ?? 0), true);
        this.listen(env.window, "lostpointercapture", (event) => {
            this.captured.delete(pointer(event).pointerId ?? 0);
            this.schedule();
        }, true);
        this.stopObserving = env.observeChanges(() => this.schedule());
    }

    private listen(target: EventTarget, type: string, listener: EventListener, capture: boolean): void {
        target.addEventListener(type, listener, capture);
        this.listeners.push([target, type, listener, capture]);
    }

    private frozen(): boolean { return this.env.locked() || this.captured.size !== 0; }

    // Layout owners can recheck the last trusted position before hover events refresh.
    elementAtPointer(): HoverElement | null {
        return !this.frozen() && this.position ? this.env.elementAt(this.position) : null;
    }

    private moved(event: PointerFields): void {
        // Locked positions are frozen; the dragged control keeps its hover.
        if (this.env.locked()) return;
        const pointerMoved = this.position?.x !== event.clientX || this.position?.y !== event.clientY;
        this.position = { x: event.clientX, y: event.clientY };
        this.refresh(pointerMoved);
    }

    private left(event: PointerFields): void {
        if (this.frozen() || event.relatedTarget) return;
        const { width, height } = this.env.viewport();
        if (event.clientX > 0 && event.clientY > 0 && event.clientX < width && event.clientY < height) return;
        this.position = null;
        this.refresh();
    }

    private schedule(): void {
        if (this.frame) return;
        this.frame = this.env.requestFrame(() => { this.frame = 0; this.refresh(); });
    }

    private refresh(pointerMoved = false): void {
        if (this.frozen()) return;
        const next: HoverElement[] = [];
        for (let element = this.position ? this.env.elementAt(this.position) : null; element; element = element.parentElement)
            next.push(element);
        this.apply(next, pointerMoved);
    }

    // Leaves innermost first and enters outermost first, like a browser does
    // for one pointer movement. Enter and leave events do not bubble.
    private apply(next: HoverElement[], pointerMoved = false): void {
        const { leaving, entering } = hoverTransition(this.chain, next);
        this.chain = next;
        for (const element of leaving) {
            element.removeAttribute(hoveredAttribute);
            if (element.isConnected) element.dispatchEvent(new Event(hoverLeaveEvent));
        }
        for (const element of entering) {
            element.setAttribute(hoveredAttribute, "");
            element.dispatchEvent(new Event(hoverEnterEvent));
        }
        this.refreshed?.(next[0] ?? null, pointerMoved);
    }

    // Leaves everything until the next pointer move, which enters its chain
    // again even if the pointer never left it. Owners that clear their own hover
    // state call this so the two cannot disagree.
    reset(): void {
        this.position = null;
        this.apply([]);
    }

    dispose(): void {
        for (const [target, type, listener, capture] of this.listeners) target.removeEventListener(type, listener, capture);
        this.stopObserving();
        if (this.frame) this.env.cancelFrame(this.frame);
        for (const element of this.chain) element.removeAttribute(hoveredAttribute);
        this.chain = [];
    }
}

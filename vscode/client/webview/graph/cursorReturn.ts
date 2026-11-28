import type { Point } from "../../src/graphScope";
import type { CursorReturnMessage, CursorReturnPolicy, CursorWarp, DragModifiers, TraceLog, WaylandPointerSample, WaylandPointerUpdate } from "../../src/graphEditorProtocol";
import type { PointerLockExitGuard } from "../../src/pointerLockExitGuard";
import type { PresentationCursor } from "../../src/presentationPlugin";
import { timelineSampleFraction } from "../../src/timelineGeometry";
import type { PresentationTime } from "../../src/timelineSamples";

export type CursorWarpAnchor = { screenOrigin: Point; scale: number };
export type CursorWindowReference = {
    windowSizeDip: { width: number; height: number };
    windowPointDip: Point;
    clientPoint: Point;
    devicePixelRatio: number;
};
export type CursorFollowTarget = {
    destination: () => Point | null | Promise<Point | null>;
    initialCenter?: Point | null;
    policy?: CursorReturnPolicy;
};
export type CursorFollow = CursorFollowTarget & (CursorWarpAnchor | { windowReference: CursorWindowReference });
export type CursorTrace = { id: string; log: TraceLog };

export function cursorWarpAnchor(event: Pick<PointerEvent, "pointerType" | "screenX" | "screenY">,
    scale: number): CursorWarpAnchor | null {
    return event.pointerType === "mouse" ? { screenOrigin: { x: event.screenX, y: event.screenY }, scale } : null;
}

export function cursorWindowReference(event: Pick<MouseEvent, "screenX" | "screenY" | "clientX" | "clientY">,
    view: Pick<Window, "screenX" | "screenY" | "outerWidth" | "outerHeight" | "devicePixelRatio">): CursorWindowReference {
    // The event relates the iframe's CSS coordinates to the browser window's DIPs.
    return { windowPointDip: { x: event.screenX - view.screenX, y: event.screenY - view.screenY },
        windowSizeDip: { width: view.outerWidth, height: view.outerHeight },
        clientPoint: { x: event.clientX, y: event.clientY }, devicePixelRatio: view.devicePixelRatio };
}

type PendingReturn = CursorFollow & { lockOrigin: Point; expiresAt?: number; resolving?: true; trace?: CursorTrace };
type WindowGeometry = Pick<Window, "screenX" | "screenY" | "outerWidth" | "outerHeight" | "devicePixelRatio">;
const returnLifetimeMs = 100;
type WaylandInput = {
    sample: (sample: WaylandPointerSample) => void;
    release: (modifiers: DragModifiers) => void;
    cancel: (reason: string) => void;
    denied: (reason: string) => void;
};

export class PointerLockCursorReturn {
    private pending: PendingReturn | null = null;
    private timer: ReturnType<typeof setTimeout> | undefined;
    private nextRequestId = 0;
    private submitted: { requestId: number; trace?: CursorTrace } | null = null;
    private waylandPress?: {
        gesture: string; reference: CursorWindowReference; view: WindowGeometry; input?: WaylandInput; granted?: true;
    };
    private nextPress?: { event: PointerEvent; start: () => void; release?: PointerEvent };
    private startingPress?: PointerEvent;
    available = false;

    get nativeDragging(): boolean { return !!this.waylandPress?.input; }
    get nativeGranted(): boolean { return this.nativeDragging && this.waylandPress?.granted === true; }
    get releasedBeforeStart(): PointerEvent | undefined { return this.startingPress; }

    constructor(private readonly guard: PointerLockExitGuard,
        private readonly locked: () => boolean,
        private readonly afterRender: (run: () => void) => void,
        private readonly send: (message: CursorReturnMessage) => void) {}

    get waitingForUnlock(): boolean { return this.pending !== null; }

    beginWayland(event: Pick<PointerEvent,
        "pointerType" | "isTrusted" | "screenX" | "screenY" | "clientX" | "clientY" | "button" | "timeStamp">,
        view: WindowGeometry, timeOrigin: number, input: WaylandInput): boolean {
        if (!this.available || event.pointerType !== "mouse" || !event.isTrusted || this.startingPress) return false;
        this.cancel("A new pointer press arrived.");
        const gesture = crypto.randomUUID();
        const reference = cursorWindowReference(event, view);
        this.waylandPress = { gesture, reference, view, input };
        this.send({ type: "beginWaylandPointer", press: { gesture, button: event.button, time: timeOrigin + event.timeStamp,
            windowSizeDip: reference.windowSizeDip, windowPointDip: reference.windowPointDip, devicePixelRatio: reference.devicePixelRatio } });
        return true;
    }

    // The next physical click reaches Chromium independently of the native
    // terminal message. Commit the old drag before entering the new control.
    deferPress(event: PointerEvent, start: () => void): boolean {
        if (!this.nativeDragging || !event.isTrusted || event.pointerType !== "mouse") return false;
        event.preventDefault();
        event.stopPropagation();
        this.nextPress = { event, start };
        return true;
    }

    notePointerUp(event: PointerEvent): boolean {
        const next = this.nextPress;
        if (!next || next.event.pointerId !== event.pointerId || next.event.button !== event.button) return false;
        next.release = event;
        return true;
    }

    receiveWayland(update: WaylandPointerUpdate): void {
        const press = this.waylandPress;
        const input = press?.input;
        if (!input || update.gesture !== press.gesture) return;
        if (update.granted) press.granted = true;
        for (const sample of update.samples ?? []) {
            input.sample(sample);
            if (this.waylandPress !== press) return;
        }
        if (!update.end) return;
        const nextPress = this.nextPress;
        this.nextPress = undefined;
        press.input = undefined;
        if (update.end.kind === "release") input.release(update.end.modifiers);
        else {
            this.cancelWayland();
            if (update.end.kind === "denied" && !press.granted && !nextPress) input.denied(update.end.reason);
            else input.cancel(update.end.reason);
        }
        if (nextPress) {
            this.cancel("A new pointer press arrived.");
            if (!nextPress.event.target || !(nextPress.event.target instanceof Element) || nextPress.event.target.isConnected) {
                this.startingPress = nextPress.release;
                try { nextPress.start(); } finally { this.startingPress = undefined; }
            }
        } else this.unlocked();
    }

    finish(release: Point | null, lockOrigin: Point | null, follow?: CursorFollow, lockError = "", trace?: CursorTrace,
        cancellation?: string): void {
        if (lockOrigin) {
            this.resetReturn("Another drag released the cursor.");
            if (!this.waylandPress?.granted) this.guard.begin(lockOrigin);
        }
        trace?.log("Finished pointer session", { release, lockOrigin, hasDestination: !!follow, lockError, cancellation });
        if (!release && cancellation) {
            this.cancelWayland();
            this.skipped(cancellation, trace);
            return;
        }
        if (!follow) {
            this.cancelWayland();
            return;
        }
        if (!release || !lockOrigin) {
            this.cancelWayland();
            this.skipped(release ? lockError || "The drag used pointer capture."
                : "The drag ended before a normal pointer-lock release.", trace);
            return;
        }
        const expiresAt = "windowReference" in follow ? undefined : Date.now() + returnLifetimeMs;
        const pending: PendingReturn = { ...follow, lockOrigin, expiresAt, trace };
        this.pending = pending;
        if (expiresAt !== undefined) this.timer = setTimeout(() => {
            if (this.pending === pending) this.cancel(pending.resolving
                ? "The release destination was unavailable before the cursor return deadline."
                : "The browser did not finish releasing pointer lock in time.");
        }, returnLifetimeMs);
    }

    unlocked(): void {
        const pending = this.pending;
        if (!pending || this.locked() || pending.resolving) return;
        pending.resolving = true;
        pending.trace?.log("Browser released pointer lock");
        this.afterRender(() => {
            if (this.pending !== pending) return;
            const center = pending.destination();
            if (center instanceof Promise) void center.then((point) => this.sendPending(pending, point),
                () => this.sendPending(pending, null));
            else this.sendPending(pending, center);
        });
    }

    private sendPending(pending: PendingReturn, center: Point | null): void {
        if (this.pending !== pending) return;
        this.clearPending();
        if ((pending.expiresAt !== undefined && Date.now() >= pending.expiresAt) || this.locked()) {
            this.cancelWayland();
            this.skipped(this.locked() ? "Another gesture acquired pointer lock."
                : "The cursor return request expired before the graph finished rendering.", pending.trace);
            return;
        }
        if (!center) {
            this.cancelWayland();
            this.skipped("The drag surface is no longer visible at release.", pending.trace);
            return;
        }
        if (center.x === pending.initialCenter?.x && center.y === pending.initialCenter?.y) {
            this.cancelWayland();
            pending.trace?.log("Destination matches the original object position", { center });
            return;
        }
        // The point's worker reply and layout can take longer than the native
        // request lifetime. Start that clock when the destination is ready.
        const expiresAt = pending.expiresAt ?? Date.now() + returnLifetimeMs;
        const press = this.waylandPress;
        if (press && (press.view.outerWidth !== press.reference.windowSizeDip.width
            || press.view.outerHeight !== press.reference.windowSizeDip.height
            || press.view.devicePixelRatio !== press.reference.devicePixelRatio)) {
            this.cancelWayland();
            this.skipped("The window geometry changed during the drag.", pending.trace);
            return;
        }
        const reference = press?.reference ?? ("windowReference" in pending ? pending.windowReference : undefined);
        if (reference) {
            const { windowSizeDip, windowPointDip, clientPoint, devicePixelRatio } = reference;
            const offsetPixels = { x: (center.x - clientPoint.x) * devicePixelRatio,
                y: (center.y - clientPoint.y) * devicePixelRatio };
            this.submit(pending, center, { destination: { windowSizeDip, windowPointDip, offsetPixels }, expiresAt,
                ...(press && { waylandGesture: press.gesture }) });
            return;
        }
        if (!("screenOrigin" in pending)) return;
        const { screenOrigin, scale, lockOrigin } = pending;
        const dx = Math.round((center.x - lockOrigin.x) * scale);
        const dy = Math.round((center.y - lockOrigin.y) * scale);
        if (!dx && !dy) {
            pending.trace?.log("Destination is already at the lock origin", { center });
            return;
        }
        // Window zoom can affect viewport scale without changing screenX/Y.
        const warp = {
            originX: Math.round(screenOrigin.x * scale), originY: Math.round(screenOrigin.y * scale),
            screenOrigin: { x: Math.round(screenOrigin.x), y: Math.round(screenOrigin.y) },
            deltaX: dx, deltaY: dy, expiresAt,
        };
        this.submit(pending, center, warp);
    }

    private submit({ policy, trace }: PendingReturn, center: Point, warp: CursorWarp): void {
        const requestId = ++this.nextRequestId;
        this.submitted = { requestId, trace };
        trace?.log("Sending cursor return to the extension", { requestId, center, warp });
        this.send({ type: "returnCursor", requestId, policy, warp, ...(trace && { traceId: trace.id }) });
    }

    complete(requestId: number): void {
        if (requestId !== this.submitted?.requestId) return;
        this.submitted.trace?.log("Webview received native cursor return acknowledgment", { requestId });
        this.submitted = null;
        // X11 processed the warp. The lock origin is no longer the place to
        // redirect events while the renderer catches up.
        this.guard.end();
    }

    cancel(reason: string): void {
        const input = this.waylandPress?.input;
        this.nextPress = undefined;
        this.cancelWayland();
        this.resetReturn(reason);
        input?.cancel(reason);
    }

    private cancelWayland(): void {
        const press = this.waylandPress;
        this.waylandPress = undefined;
        if (press) this.send({ type: "cancelWaylandCursor", gesture: press.gesture });
    }

    private resetReturn(reason: string): void {
        this.submitted = null;
        if (this.pending) this.skipped(reason, this.pending.trace);
        this.clearPending();
    }

    private clearPending(): void {
        clearTimeout(this.timer);
        this.timer = undefined;
        this.pending = null;
    }

    private skipped(detail: string, trace?: CursorTrace): void {
        if (trace) trace.log("Cursor return skipped", { reason: detail });
        else this.send({ type: "cursorReturnSkipped", detail });
    }
}

export function presentationCursorPoint(destination: PresentationCursor, bounds: Pick<DOMRect, "left" | "top" | "width" | "height">,
    time?: PresentationTime): Point | null {
    if (!("sample" in destination)) return { x: bounds.left + destination.x, y: bounds.top + destination.y };
    if (!time || !(time.spanSamples > 0)) return null;
    return { x: bounds.left + timelineSampleFraction(BigInt(destination.sample), time) * bounds.width,
        y: bounds.top + destination.yFraction * bounds.height };
}

export function visibleCursorDestination(surface: Element, position: Point): Point | null {
    const rect = surface.getBoundingClientRect();
    const timeline = surface.closest(".timeline-scope");
    const viewport = timeline?.getBoundingClientRect();
    const heading = timeline?.querySelector(":scope > .timeline-heading");
    const headingBottom = heading && !heading.contains(surface) ? heading.getBoundingClientRect().bottom : 0;
    const left = Math.max(0, rect.left, viewport?.left ?? 0);
    const right = Math.min(window.innerWidth, rect.right, viewport?.right ?? Infinity);
    const top = Math.max(0, rect.top, viewport?.top ?? 0, headingBottom);
    const bottom = Math.min(window.innerHeight, rect.bottom, viewport?.bottom ?? Infinity);
    if (position.x >= left && position.x < right && position.y >= top && position.y < bottom) return position;
    if (right - left < 4 || bottom - top < 4 || !Number.isFinite(position.x) || !Number.isFinite(position.y)) return null;
    return { x: Math.max(left + 2, Math.min(right - 2, position.x)),
        y: Math.max(top + 2, Math.min(bottom - 2, position.y)) };
}

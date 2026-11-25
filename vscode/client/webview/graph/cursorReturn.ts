import type { Point } from "../../src/graphScope";
import type { CursorReturnMessage, CursorReturnPolicy, CursorWarp, TraceLog } from "../../src/graphEditorProtocol";
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
const returnLifetimeMs = 100;

export class PointerLockCursorReturn {
    private pending: PendingReturn | null = null;
    private timer: ReturnType<typeof setTimeout> | undefined;
    private nextRequestId = 0;
    private submitted: { requestId: number; trace?: CursorTrace } | null = null;

    constructor(private readonly guard: PointerLockExitGuard,
        private readonly locked: () => boolean,
        private readonly afterRender: (run: () => void) => void,
        private readonly send: (message: CursorReturnMessage) => void) {}

    get waitingForUnlock(): boolean { return this.pending !== null; }

    finish(release: Point | null, lockOrigin: Point | null, follow?: CursorFollow, lockError = "", trace?: CursorTrace,
        cancellation?: string): void {
        if (lockOrigin) {
            this.cancel("Another drag released the cursor.");
            this.guard.begin(lockOrigin);
        }
        trace?.log("Finished pointer session", { release, lockOrigin, hasDestination: !!follow, lockError, cancellation });
        if (!release && cancellation) {
            this.skipped(cancellation, trace);
            return;
        }
        if (!follow) return;
        if (!release || !lockOrigin) {
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
            this.skipped(this.locked() ? "Another gesture acquired pointer lock."
                : "The cursor return request expired before the graph finished rendering.", pending.trace);
            return;
        }
        if (!center) {
            this.skipped("The drag surface is no longer visible at release.", pending.trace);
            return;
        }
        if (center.x === pending.initialCenter?.x && center.y === pending.initialCenter?.y) {
            pending.trace?.log("Destination matches the original object position", { center });
            return;
        }
        // The point's worker reply and layout can take longer than the native
        // request lifetime. Start that clock when the destination is ready.
        const expiresAt = pending.expiresAt ?? Date.now() + returnLifetimeMs;
        if ("windowReference" in pending) {
            const { windowSizeDip, windowPointDip, clientPoint, devicePixelRatio } = pending.windowReference;
            const offsetPixels = { x: (center.x - clientPoint.x) * devicePixelRatio,
                y: (center.y - clientPoint.y) * devicePixelRatio };
            this.submit(pending, center, { destination: { windowSizeDip, windowPointDip, offsetPixels }, expiresAt });
            return;
        }
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

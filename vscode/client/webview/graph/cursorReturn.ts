import type { Point } from "../../src/graphScope";
import type { CursorReturnMessage, CursorReturnPolicy, TraceLog } from "../../src/graphEditorProtocol";
import type { PointerLockExitGuard } from "../../src/pointerLockExitGuard";

export type CursorWarpAnchor = { screenOrigin: Point; scale: number };
export type CursorFollowTarget = {
    destination: () => Point | null | Promise<Point | null>;
    initialCenter?: Point | null;
    policy?: CursorReturnPolicy;
};
export type CursorFollow = CursorWarpAnchor & CursorFollowTarget;
export type CursorTrace = { id: string; log: TraceLog };

export function cursorWarpAnchor(event: Pick<PointerEvent, "pointerType" | "screenX" | "screenY">,
    scale: number): CursorWarpAnchor | null {
    return event.pointerType === "mouse" ? { screenOrigin: { x: event.screenX, y: event.screenY }, scale } : null;
}

type PendingReturn = CursorFollow & { lockOrigin: Point; expiresAt: number; resolving?: true; trace?: CursorTrace };
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

    finish(release: Point | null, lockOrigin: Point | null, follow?: CursorFollow, lockError = "", trace?: CursorTrace): void {
        if (lockOrigin) {
            this.cancel("Another drag released the cursor.");
            this.guard.begin(lockOrigin);
        }
        trace?.log("Finished pointer session", { release, lockOrigin, hasDestination: !!follow, lockError });
        if (!follow) return;
        if (!release || !lockOrigin) {
            this.skipped(release ? lockError || "The drag used pointer capture."
                : "The drag ended before a normal pointer-lock release.", trace);
            return;
        }
        const pending: PendingReturn = { ...follow, lockOrigin, expiresAt: Date.now() + returnLifetimeMs, trace };
        this.pending = pending;
        this.timer = setTimeout(() => {
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
        if (Date.now() >= pending.expiresAt || this.locked()) {
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
        const { screenOrigin, scale, lockOrigin, expiresAt, policy } = pending;
        const dx = Math.round((center.x - lockOrigin.x) * scale);
        const dy = Math.round((center.y - lockOrigin.y) * scale);
        if (!dx && !dy) {
            pending.trace?.log("Destination is already at the lock origin", { center });
            return;
        }
        const requestId = ++this.nextRequestId;
        this.submitted = { requestId, trace: pending.trace };
        // Window zoom can affect viewport scale without changing screenX/Y.
        const warp = {
            originX: Math.round(screenOrigin.x * scale), originY: Math.round(screenOrigin.y * scale),
            screenOrigin: { x: Math.round(screenOrigin.x), y: Math.round(screenOrigin.y) },
            deltaX: dx, deltaY: dy, expiresAt,
        };
        pending.trace?.log("Sending cursor return to the extension", { requestId, center, warp });
        this.send({ type: "returnCursor", requestId, policy, warp, ...(pending.trace && { traceId: pending.trace.id }) });
    }

    complete(requestId: number): void {
        if (requestId !== this.submitted?.requestId) return;
        this.submitted.trace?.log("Webview received native move confirmation", { requestId });
        this.submitted = null;
        // The native cursor has moved. The lock origin is no longer a safe
        // place to redirect events while the renderer catches up.
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
    if (position.x >= left && position.x < right && position.y >= top && position.y < bottom
        && surface.contains(document.elementFromPoint(position.x, position.y))) return position;
    if (right - left < 4 || bottom - top < 4 || !Number.isFinite(position.x) || !Number.isFinite(position.y)) return null;
    let x = Math.max(left + 2, Math.min(right - 2, position.x));
    const y = Math.max(top + 2, Math.min(bottom - 2, position.y));
    const inward = position.x < left ? 1 : position.x >= right ? -1 : 0;
    while (x >= left + 2 && x <= right - 2) {
        const hit = document.elementFromPoint(x, y);
        if (surface.contains(hit)) return { x, y };
        if (!hit || !inward) return null;
        // The resize handle or ruler settings can cover the first clamped
        // point. Move inward past the covering element at the same height.
        const cover = hit.getBoundingClientRect();
        x = inward > 0 ? Math.max(x + 2, cover.right + 2) : Math.min(x - 2, cover.left - 2);
    }
    return null;
}

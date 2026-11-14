import type { Point } from "./graphScope";

type Host = { request: () => void | Promise<void>; exit: () => void; locked: () => boolean };
type Session = { pointerId: number; origin: Point; pending: boolean; granted: boolean; mouse: boolean;
    lockedPosition?: Point; positionWarning?: boolean };

// The webview owns the DOM listeners; this owner keeps asynchronous lock
// acquisition tied to one drag, including release before acquisition finishes.
export class KnobPointerLock {
    private session: Session | null = null;
    private requesting: { session: Session } | null = null;
    private exiting = false;
    constructor(private readonly host: Host,
        private readonly moved: (pointerId: number, delta: number) => void,
        private readonly finished: (pointerId: number, point: Point | null) => void,
        private readonly failed: (message: string) => void) {}

    start(pointerId: number, origin: Point, mouse = true): void {
        const session: Session = { pointerId, origin, pending: mouse, granted: false, mouse };
        this.session = session;
        if (!mouse) return;
        if (this.host.locked() && !this.exiting) { session.pending = false; session.granted = true; return; }
        this.request();
    }
    private request(): void {
        if (this.requesting || this.exiting || !this.session?.mouse) return;
        const token = { session: this.session };
        this.requesting = token;
        try {
            const request = this.host.request();
            if (request) void request.catch((error: unknown) => {
                if (this.requesting === token) this.error(error instanceof Error ? `${error.name}: ${error.message}` : String(error));
            });
        } catch (error) {
            this.error(error instanceof Error ? `${error.name}: ${error.message}` : String(error));
        }
    }
    retryAfterFocus(): void {
        const session = this.session;
        if (!session?.mouse || session.granted || session.pending) return;
        session.pending = true;
        this.request();
    }
    changed(): void {
        const session = this.session;
        if (this.host.locked()) {
            this.requesting = null;
            if (this.exiting) return;
            if (!session?.mouse) { this.exit(); return; }
            session.pending = false; session.granted = true;
        } else if (this.exiting) {
            // Release and the next press can precede the browser's unlock
            // event. That old event must not terminate the new drag.
            this.exiting = false;
            this.request();
        } else if (session?.granted) {
            this.session = null;
            this.finished(session.pointerId, null);
        }
    }
    error(message = "Pointer lock was denied; dragging remains limited to the screen bounds."): void {
        const requestedSession = this.requesting?.session;
        this.requesting = null;
        if (!this.session?.pending) return;
        if (requestedSession && requestedSession !== this.session) {
            this.request();
            return;
        }
        this.session.pending = false;
        this.failed(message);
    }
    move(delta: number, position?: Point): boolean {
        if (!this.session?.granted || this.exiting || !this.host.locked()) return false;
        if (position) {
            const anchor = this.session.lockedPosition;
            if (!anchor) this.session.lockedPosition = position;
            else if (!this.session.positionWarning && Math.hypot(position.x - anchor.x, position.y - anchor.y) > 2) {
                this.session.positionWarning = true;
                this.failed(`Browser reports pointer lock but client coordinates moved: ${JSON.stringify({ anchor, position })}`);
            }
        }
        this.moved(this.session.pointerId, delta);
        return true;
    }
    status() {
        return { locked: this.host.locked(), pending: this.session?.pending ?? false,
            granted: this.session?.granted ?? false, exiting: this.exiting,
            origin: this.session?.origin ?? null };
    }
    release(pointerId: number, point: Point): void {
        const session = this.session;
        if (!session || session.pointerId !== pointerId) return;
        const position = session.granted || this.host.locked() ? session.origin : point;
        this.session = null;
        this.finished(pointerId, position);
        this.exit();
    }
    cancel(): void {
        const session = this.session;
        this.session = null;
        if (session) this.finished(session.pointerId, null);
        this.exit();
    }
    private exit(): void {
        if (this.exiting || !this.host.locked()) return;
        this.exiting = true;
        this.host.exit();
    }
    suppressPointerCancellation(): boolean { return !!this.session && (this.session.pending || this.session.granted); }
}

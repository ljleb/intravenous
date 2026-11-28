import type { Point } from "./graphScope";

type Host = { request: () => void | Promise<void>; exit: () => void; locked: () => boolean };
type Session = { pointerId: number; origin: Point; pending: boolean; granted: boolean; mouse: boolean };

// The webview owns the DOM listeners; this owner keeps asynchronous lock
// acquisition tied to one drag, including release before acquisition finishes.
export class KnobPointerLock<Delta = number> {
    private session: Session | null = null;
    private requesting: { session: Session } | null = null;
    private exiting = false;
    constructor(private readonly host: Host,
        private readonly moved: (pointerId: number, delta: Delta) => void,
        // lockOrigin is set when a granted pointer lock ends with the drag: the
        // visible cursor returns there, whatever position the host reports next.
        private readonly finished: (pointerId: number, point: Point | null, lockOrigin: Point | null, cancellation?: string) => void,
        private readonly failed: (message: string) => void) {}

    start(pointerId: number, origin: Point, mouse = true): void {
        if (this.session) return;
        const session: Session = { pointerId, origin, pending: mouse, granted: false, mouse };
        this.session = session;
        if (!mouse) return;
        if (this.host.locked()) { this.exit(); return; }
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
            const requestedSession = this.requesting?.session;
            this.requesting = null;
            if (this.exiting) return;
            // A late grant belongs to the press that requested it, including
            // its physical cursor anchor. Never adopt it for a later press.
            if (requestedSession && requestedSession !== session) { this.exit(); return; }
            if (!session?.mouse) { this.exit(); return; }
            session.pending = false; session.granted = true;
        } else if (this.exiting) {
            // Release and the next press can precede the browser's unlock
            // event. That old event must not terminate the new drag.
            this.exiting = false;
            this.request();
        } else if (session?.granted) {
            this.session = null;
            this.finished(session.pointerId, null, session.origin, "The browser released pointer lock before the editor received a button release.");
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
    move(delta: Delta, held?: { buttons: number; mask: number }): boolean {
        if (!this.session?.granted || this.exiting || !this.host.locked()) return false;
        if (held && !(held.buttons & held.mask)) this.release(this.session.pointerId, this.session.origin);
        else this.moved(this.session.pointerId, delta);
        return true;
    }
    moveNative(pointerId: number, delta: Delta): void {
        if (this.session?.pointerId === pointerId) this.moved(pointerId, delta);
    }

    releaseNative(pointerId: number): void {
        const session = this.session;
        if (!session || session.pointerId !== pointerId) return;
        this.session = null;
        this.finished(pointerId, session.origin, session.origin);
    }

    status() {
        return { locked: this.host.locked(), pending: this.session?.pending ?? false,
            granted: this.session?.granted ?? false, exiting: this.exiting,
            origin: this.session?.origin ?? null };
    }
    release(pointerId: number, point: Point): void {
        const session = this.session;
        if (!session || session.pointerId !== pointerId) return;
        // The browser can report a locked element before delivering the
        // change event. Release in that interval also restores the origin.
        const locked = this.host.locked();
        const origin = session.granted || (locked && this.requesting?.session === session) ? session.origin : null;
        this.session = null;
        try {
            this.finished(pointerId, origin ?? point, origin);
        } finally {
            this.exit();
        }
    }
    cancel(reason?: string): void {
        const session = this.session;
        this.session = null;
        try {
            if (session) {
                const ownsLock = session.granted || (this.host.locked() && this.requesting?.session === session);
                this.finished(session.pointerId, null, ownsLock ? session.origin : null, reason);
            }
        } finally {
            this.exit();
        }
    }
    private exit(): void {
        if (this.exiting || !this.host.locked()) return;
        this.requesting = null;
        this.exiting = true;
        this.host.exit();
    }
    suppressPointerCancellation(): boolean { return !!this.session && (this.session.pending || this.session.granted); }
}

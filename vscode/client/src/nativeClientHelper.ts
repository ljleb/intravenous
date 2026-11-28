import { spawn, type ChildProcess } from "node:child_process";
import { Duplex } from "node:stream";
import { JsonRpcSocketClient } from "./rpcClient";
import type { CursorWarp, CursorWindowDestination, TraceLog, WaylandPointerPress, WaylandPointerUpdate } from "./graphEditorProtocol";

const initialRestartDelayMs = 1000;
const maximumRestartDelayMs = 30000;
const stableRunMs = 30000;
const readyTimeoutMs = 10000;

type RunningHelper = { child: ChildProcess; socket: Duplex | null; rpc: JsonRpcSocketClient | null;
    readyTimer?: NodeJS.Timeout; readyAt?: number; cursorWarp: boolean; cursorWarpResult: boolean; waylandPointer: boolean;
    gestures: Map<string, (update: WaylandPointerUpdate) => void> };

function validWindowDestination(destination: CursorWindowDestination | undefined): boolean {
    const size = destination?.windowSizeDip;
    const point = destination?.windowPointDip;
    const offset = destination?.offsetPixels;
    return !!size && Number.isSafeInteger(size.width) && size.width > 0 && Number.isSafeInteger(size.height) && size.height > 0
        && !!point && Number.isFinite(point.x) && Number.isFinite(point.y)
        && !!offset && Number.isFinite(offset.x) && Number.isFinite(offset.y);
}

function warpCoordinates(request: CursorWarp) {
    if ("destination" in request) {
        if (!validWindowDestination(request.destination)
            || (request.waylandGesture !== undefined && !validWaylandGesture(request.waylandGesture))) return;
        return { destination: request.destination, waylandGesture: request.waylandGesture };
    }
    const { originX, originY, screenOrigin, deltaX, deltaY } = request;
    if (![originX, originY, deltaX, deltaY].every(Number.isSafeInteger)
        || (screenOrigin !== undefined && (!Number.isSafeInteger(screenOrigin?.x) || !Number.isSafeInteger(screenOrigin?.y)))
        || Math.abs(deltaX) > 32767 || Math.abs(deltaY) > 32767) return;
    return { originX, originY, deltaX, deltaY,
        ...(screenOrigin && { screenOrigin: { x: screenOrigin.x, y: screenOrigin.y } }) };
}

export function validWaylandGesture(gesture: unknown): gesture is string {
    return typeof gesture === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(gesture);
}

// The local extension host owns this process. Each launch has its own
// inherited connection and requests from that connection are never replayed.
export class NativeClientHelper {
    private running: RunningHelper | null = null;
    private restartTimer: NodeJS.Timeout | null = null;
    private restartDelayMs = initialRestartDelayMs;
    private disposed = false;
    private readonly pointerListeners = new Set<(available: boolean) => void>();

    get waylandPointerAvailable(): boolean { return this.running?.waylandPointer ?? false; }

    onWaylandPointerAvailable(listener: (available: boolean) => void): { dispose(): void } {
        this.pointerListeners.add(listener);
        return { dispose: () => { this.pointerListeners.delete(listener); } };
    }

    private setWaylandPointerAvailable(run: RunningHelper, available: boolean): void {
        if (run.waylandPointer === available) return;
        run.waylandPointer = available;
        if (!available) {
            for (const [gesture, update] of run.gestures)
                update({ gesture, end: { kind: "cancel", reason: "The native drag connection closed." } });
            run.gestures.clear();
        }
        for (const listener of this.pointerListeners) listener(available);
    }

    constructor(private readonly launch: () => ChildProcess, private readonly log: (message: string) => void) {}

    static launch(binary: string): ChildProcess {
        return spawn(binary, ["--rpc-fd", "3", "--owner-pid", String(process.pid)],
            { stdio: ["ignore", "ignore", "pipe", "pipe"] });
    }

    start(): void {
        if (this.disposed || this.running || this.restartTimer) return;
        let child: ChildProcess;
        try { child = this.launch(); }
        catch (error) {
            this.scheduleRestart(error instanceof Error ? error.message : String(error));
            return;
        }
        const socket = child.stdio[3] instanceof Duplex ? child.stdio[3] : null;
        const run: RunningHelper = { child, socket, rpc: null, cursorWarp: false, cursorWarpResult: false, waylandPointer: false,
            gestures: new Map() };
        this.running = run;
        let failure = "";
        child.stderr?.setEncoding("utf8").on("data", (chunk: string) => {
            const detail = chunk.trim();
            if (this.running === run && detail) this.log(`Native helper: ${detail}`);
        });
        child.on("error", (error) => { failure ||= error.message; });
        child.once("close", (code, signal) => {
            if (this.running !== run) return;
            this.running = null;
            this.closeConnection(run);
            this.scheduleRestart(failure || `exited (code=${code}, signal=${signal})`, run.readyAt);
        });
        const stop = (reason: string) => {
            if (this.running !== run) return;
            failure ||= reason;
            this.closeConnection(run);
            child.kill("SIGTERM");
        };
        if (!socket) {
            stop("inherited RPC connection missing");
            return;
        }
        run.rpc = new JsonRpcSocketClient(socket, (method, params) => {
            if (this.running !== run || !run.rpc) return;
            if (method === "native.waylandPointerAvailable") {
                this.setWaylandPointerAvailable(run, params.available === true);
                return;
            }
            if (method === "native.waylandPointerUpdate" && validWaylandGesture(params.gesture)) {
                const update = params as WaylandPointerUpdate;
                const receive = run.gestures.get(update.gesture);
                if (update.end) run.gestures.delete(update.gesture);
                receive?.(update);
                return;
            }
            if (method !== "native.ready" || run.readyAt !== undefined) return;
            clearTimeout(run.readyTimer);
            run.readyAt = Date.now();
            run.cursorWarp = params.cursorWarp === true;
            run.cursorWarpResult = params.cursorWarpResult === true;
            if (!run.cursorWarp && params.waylandCandidate !== true) {
                const reason = typeof params.cursorWarpUnavailableReason === "string" ? params.cursorWarpUnavailableReason
                    : "The native helper reported no cursor warp support for the current display.";
                this.log(`Native cursor return is unavailable. ${reason}`);
            }
        }, (error) => { if (run.rpc) stop(error.message); });
        run.readyTimer = setTimeout(() => stop("timed out waiting for readiness"), readyTimeoutMs);
        void run.rpc.connect().catch((error: Error) => stop(error.message));
    }

    beginWaylandPointer(press: WaylandPointerPress, receive: (update: WaylandPointerUpdate) => void): boolean {
        const run = this.running;
        if (!run?.waylandPointer || !run.rpc || !run.socket?.writable || run.socket.writableNeedDrain
            || !validWaylandGesture(press.gesture)) return false;
        run.gestures.set(press.gesture, receive);
        run.rpc.notify("native.beginWaylandPointer", press);
        return true;
    }

    cancelWaylandCursor(gesture: string): void {
        const run = this.running;
        run?.gestures.delete(gesture);
        if (run?.waylandPointer && validWaylandGesture(gesture)) run.rpc?.notify("native.cancelWaylandCursor", { gesture });
    }

    warpCursor(request: CursorWarp, { hideCursor = false, followPointer = false, completed, trace }: {
        hideCursor?: boolean; followPointer?: boolean; completed?: () => void; trace?: TraceLog;
    } = {}): void {
        const run = this.running;
        const now = Date.now();
        if (!request || typeof request !== "object") return;
        const skipped = (detail: string) => {
            trace?.("Native cursor return skipped", { reason: detail });
            this.log(`Cursor return skipped. ${detail}`);
        };
        const wayland = "destination" in request && !!request.waylandGesture;
        const available = wayland ? run?.waylandPointer : run?.cursorWarp;
        if (!available || !run?.rpc || !run.socket?.writable || run.socket.writableNeedDrain) {
            trace?.("Native helper unavailable", { launched: !!run, ready: run?.readyAt !== undefined,
                cursorWarp: run?.cursorWarp, writable: run?.socket?.writable, backpressure: run?.socket?.writableNeedDrain });
            if (run?.readyAt === undefined || run.cursorWarp)
                skipped("The helper is not ready for cursor return.");
            return;
        }
        const { expiresAt } = request;
        const coordinates = warpCoordinates(request);
        if (!coordinates || !Number.isSafeInteger(expiresAt) || expiresAt <= now || expiresAt > now + 100) {
            skipped(`The native request expired or its coordinates were invalid. Remaining time: ${expiresAt - now} ms.`);
            return;
        }
        const params = { ...coordinates, expiresAt,
            ...(hideCursor && { hideCursor: true }),
            ...(followPointer && { followPointer: true }),
            ...(trace && { trace: true }) };
        const expectsReply = !wayland && !!completed && run.cursorWarpResult;
        trace?.("Native warp dispatched", { hideCursor, followPointer, expectsReply, expiresInMs: expiresAt - now });
        if (!expectsReply) { run.rpc.notify("native.warpCursor", params); return; }
        void run.rpc.request<{ moved: boolean }>("native.warpCursor", params).then((result) => {
            const elapsedMs = Date.now() - now;
            const confirmed = result?.moved === true && this.running === run && now + elapsedMs < expiresAt;
            trace?.("Native warp replied", { moved: result?.moved, confirmed, elapsedMs });
            if (confirmed) completed?.();
        }).catch((error) => skipped(`The helper could not confirm cursor return. ${String(error)}`));
    }

    private closeConnection(run: RunningHelper): void {
        clearTimeout(run.readyTimer);
        const rpc = run.rpc;
        run.rpc = null;
        run.cursorWarp = false;
        this.setWaylandPointerAvailable(run, false);
        rpc?.dispose();
    }

    private scheduleRestart(reason: string, readyAt?: number): void {
        if (this.disposed) return;
        const stable = readyAt !== undefined && Date.now() - readyAt >= stableRunMs;
        if (stable) this.restartDelayMs = initialRestartDelayMs;
        const delay = this.restartDelayMs;
        this.restartDelayMs = Math.min(maximumRestartDelayMs, delay * 2);
        this.log(`Native helper ${reason}. Restarting in ${delay} ms.`);
        this.restartTimer = setTimeout(() => {
            this.restartTimer = null;
            this.start();
        }, delay);
    }

    dispose(): void {
        this.disposed = true;
        if (this.restartTimer) clearTimeout(this.restartTimer);
        this.restartTimer = null;
        const run = this.running;
        this.running = null;
        if (!run) return;
        this.closeConnection(run);
        run.child.kill("SIGTERM");
    }
}

import { spawn, type ChildProcess } from "node:child_process";
import { Duplex } from "node:stream";
import { JsonRpcSocketClient } from "./rpcClient";
import type { CursorWarp, CursorWindowDestination, TraceLog } from "./graphEditorProtocol";

const initialRestartDelayMs = 1000;
const maximumRestartDelayMs = 30000;
const stableRunMs = 30000;
const readyTimeoutMs = 10000;

type RunningHelper = { child: ChildProcess; socket: Duplex | null; rpc: JsonRpcSocketClient | null;
    readyTimer?: NodeJS.Timeout; readyAt?: number; cursorWarp: boolean; cursorWarpResult: boolean };

function validWindowDestination(destination: CursorWindowDestination | undefined): boolean {
    const size = destination?.windowSizeDip;
    const point = destination?.windowPointDip;
    const offset = destination?.offsetPixels;
    return !!size && Number.isSafeInteger(size.width) && size.width > 0 && Number.isSafeInteger(size.height) && size.height > 0
        && !!point && Number.isFinite(point.x) && Number.isFinite(point.y)
        && !!offset && Number.isFinite(offset.x) && Number.isFinite(offset.y);
}

function warpCoordinates(request: CursorWarp) {
    if ("destination" in request)
        return validWindowDestination(request.destination) ? { destination: request.destination } : undefined;
    const { originX, originY, screenOrigin, deltaX, deltaY } = request;
    if (![originX, originY, deltaX, deltaY].every(Number.isSafeInteger)
        || (screenOrigin !== undefined && (!Number.isSafeInteger(screenOrigin?.x) || !Number.isSafeInteger(screenOrigin?.y)))
        || Math.abs(deltaX) > 32767 || Math.abs(deltaY) > 32767) return;
    return { originX, originY, deltaX, deltaY,
        ...(screenOrigin && { screenOrigin: { x: screenOrigin.x, y: screenOrigin.y } }) };
}

// The local extension host owns this process. Each launch has its own
// inherited connection and requests from that connection are never replayed.
export class NativeClientHelper {
    private running: RunningHelper | null = null;
    private restartTimer: NodeJS.Timeout | null = null;
    private restartDelayMs = initialRestartDelayMs;
    private disposed = false;

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
        const run: RunningHelper = { child, socket, rpc: null, cursorWarp: false, cursorWarpResult: false };
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
            if (this.running !== run || !run.rpc || method !== "native.ready" || run.readyAt !== undefined) return;
            clearTimeout(run.readyTimer);
            run.readyAt = Date.now();
            run.cursorWarp = params.cursorWarp === true;
            run.cursorWarpResult = params.cursorWarpResult === true;
            if (!run.cursorWarp) {
                const reason = typeof params.cursorWarpUnavailableReason === "string" ? params.cursorWarpUnavailableReason
                    : "The native helper reported no cursor warp support for the current display.";
                this.log(`Native cursor return is unavailable. ${reason}`);
            }
        }, (error) => { if (run.rpc) stop(error.message); });
        run.readyTimer = setTimeout(() => stop("timed out waiting for readiness"), readyTimeoutMs);
        void run.rpc.connect().catch((error: Error) => stop(error.message));
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
        if (!run?.cursorWarp || !run.rpc || !run.socket?.writable || run.socket.writableNeedDrain) {
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
        const expectsReply = !!completed && run.cursorWarpResult;
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

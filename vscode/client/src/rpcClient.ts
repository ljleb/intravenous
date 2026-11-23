import * as net from "net";
import { Duplex } from "stream";
import type { JsonRpcSend } from "./clientInteractionTrace";

export type JsonRpcNotificationHandler =
    ((method: string, params: Record<string, unknown>) => void) | null;
export type JsonRpcDisconnectHandler = ((error: Error) => void) | null;
export type JsonRpcRevisionHandler = ((revision: number) => void) | null;

type PendingRequest = {
    resolve: (value: RpcReply) => void;
    reject: (error: Error) => void;
};

type ResultChunk = { transferId: string; offset: number; nextOffset: number | null; text: string };
type RpcReply = { id?: number; result?: unknown; resultChunk?: ResultChunk;
    error?: { message?: string; data?: { currentRevision?: number } } };

export class JsonRpcSocketClient {
    private readonly socketPath: string | null;
    private readonly notificationHandler: JsonRpcNotificationHandler;
    private readonly disconnectHandler: JsonRpcDisconnectHandler;
    private readonly revisionHandler: JsonRpcRevisionHandler;
    private socket: Duplex | null = null;
    private nextId = 1;
    private readonly pending = new Map<number, PendingRequest>();
    private buffer = "";
    private socketInitialized = false;
    private disconnectReported = false;
    private readonly sendObservers = new Set<(send: JsonRpcSend) => void>();

    observeSends(observe: (send: JsonRpcSend) => void): () => void {
        this.sendObservers.add(observe);
        return () => { this.sendObservers.delete(observe); };
    }

    constructor(
        socketPathOrSocket: string | Duplex,
        notificationHandler: JsonRpcNotificationHandler = null,
        disconnectHandler: JsonRpcDisconnectHandler = null,
        revisionHandler: JsonRpcRevisionHandler = null,
    ) {
        this.socketPath = typeof socketPathOrSocket === "string" ? socketPathOrSocket : null;
        this.notificationHandler = notificationHandler;
        this.disconnectHandler = disconnectHandler;
        this.revisionHandler = revisionHandler;
        if (typeof socketPathOrSocket !== "string") {
            this.socket = socketPathOrSocket;
        }
    }

    async connect(timeoutMs = 10000): Promise<void> {
        if (this.socket) {
            this.initializeSocket(this.socket);
            return;
        }

        await new Promise<void>((resolve, reject) => {
            const socket = net.createConnection(this.socketPath!);
            const timeout = setTimeout(() => {
                socket.destroy();
                reject(new Error(`timed out connecting to ${this.socketPath}`));
            }, timeoutMs);

            socket.on("connect", () => {
                clearTimeout(timeout);
                this.initializeSocket(socket);
                resolve();
            });

            socket.on("error", (error) => {
                clearTimeout(timeout);
                reject(error);
            });
        });
    }

    private initializeSocket(socket: Duplex): void {
        if (this.socketInitialized) {
            return;
        }
        this.socket = socket;
        this.socketInitialized = true;
        if ("setEncoding" in socket && typeof socket.setEncoding === "function") {
            socket.setEncoding("utf8");
        }
        socket.on("data", (chunk: Buffer | string) => this.onData(String(chunk)));
        socket.on("error", (error) => this.reportDisconnect(error));
        socket.on("close", () => this.reportDisconnect(
            new Error("Server connection closed"),
        ));
    }

    async request<T = unknown>(method: string, params: Record<string, unknown>): Promise<T> {
        let reply = await this.sendRequest(method, params);
        let chunk = reply.resultChunk;
        if (chunk) {
            const id = reply.id;
            const { transferId } = chunk;
            if (typeof transferId !== "string" || !transferId) throw new Error("Invalid RPC result transfer ID");
            const text: string[] = [];
            let offset = 0;
            for (;;) {
                if (!chunk || chunk.transferId !== transferId || chunk.offset !== offset || typeof chunk.text !== "string" || !chunk.text)
                    throw new Error("Invalid RPC result chunk");
                text.push(chunk.text);
                if (chunk.nextOffset === null) break;
                offset += Buffer.byteLength(chunk.text, "utf8");
                if (chunk.nextOffset !== offset) throw new Error("Invalid RPC result offset");
                chunk = this.requireSuccess(await this.sendRequest("session.readResult", { transferId, offset })).resultChunk;
            }
            reply = JSON.parse(text.join(""));
            if (reply.id !== id) throw new Error("RPC result ID changed during transfer");
        }
        return this.requireSuccess(reply).result as T;
    }

    private requireSuccess(reply: RpcReply): RpcReply {
        const revision = (reply.result as { sessionRevision?: number } | undefined)?.sessionRevision
            ?? reply.error?.data?.currentRevision;
        if (typeof revision === "number") this.revisionHandler?.(revision);
        if (reply.error) throw new Error(reply.error.message || "unknown JSON-RPC error");
        return reply;
    }

    private sendRequest(method: string, params: Record<string, unknown>): Promise<RpcReply> {
        const id = this.nextId++;
        const payload = JSON.stringify({
            jsonrpc: "2.0",
            id,
            method,
            params,
        }) + "\n";

        if (!this.socket) {
            throw new Error("JSON-RPC client is not connected");
        }

        return new Promise<RpcReply>((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.socket!.write(payload, "utf8");
            for (const observe of this.sendObservers) observe({ method, params, notification: false });
        });
    }

    // A JSON-RPC notification: no id, so the server sends no reply. Used only
    // for transient latest values whose loss a later request makes harmless.
    notify(method: string, params: Record<string, unknown>): void {
        if (!this.socket) {
            throw new Error("JSON-RPC client is not connected");
        }
        this.socket.write(JSON.stringify({ jsonrpc: "2.0", method, params }) + "\n", "utf8");
        for (const observe of this.sendObservers) observe({ method, params, notification: true });
    }

    private onData(chunk: string): void {
        this.buffer += chunk;
        for (;;) {
            const newline = this.buffer.indexOf("\n");
            if (newline === -1) {
                return;
            }

            const line = this.buffer.slice(0, newline).trim();
            this.buffer = this.buffer.slice(newline + 1);
            if (!line) {
                continue;
            }

            let message: any;
            try {
                message = JSON.parse(line);
            } catch (error: any) {
                this.failPending(new Error(`failed to parse server response: ${error.message}`));
                return;
            }

            if (typeof message.id !== "number") {
                if (typeof message.method === "string" && this.notificationHandler) {
                    this.notificationHandler(message.method, message.params || {});
                }
                continue;
            }

            const pending = this.pending.get(message.id);
            if (!pending) {
                continue;
            }
            this.pending.delete(message.id);

            pending.resolve(message);
        }
    }

    private failPending(error: Error): void {
        for (const { reject } of this.pending.values()) {
            reject(error);
        }
        this.pending.clear();
    }

    private reportDisconnect(error: Error): void {
        this.failPending(error);
        if (this.disconnectReported) return;
        this.disconnectReported = true;
        this.disconnectHandler?.(error);
    }

    dispose(): void {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
        this.failPending(new Error("Client disposed"));
    }
}

import * as net from "net";
import { Duplex } from "stream";

export type JsonRpcNotificationHandler =
    ((method: string, params: Record<string, unknown>) => void) | null;

type PendingRequest = {
    resolve: (value: unknown) => void;
    reject: (error: Error) => void;
};

export class JsonRpcSocketClient {
    private readonly socketPath: string | null;
    private readonly notificationHandler: JsonRpcNotificationHandler;
    private socket: Duplex | null = null;
    private nextId = 1;
    private readonly pending = new Map<number, PendingRequest>();
    private buffer = "";
    private socketInitialized = false;

    constructor(socketPathOrSocket: string | Duplex, notificationHandler: JsonRpcNotificationHandler = null) {
        this.socketPath = typeof socketPathOrSocket === "string" ? socketPathOrSocket : null;
        this.notificationHandler = notificationHandler;
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
        socket.on("error", (error) => this.failPending(error));
        socket.on("close", () => this.failPending(new Error("Intravenous server connection closed")));
    }

    async request<T = unknown>(method: string, params: Record<string, unknown>): Promise<T> {
        const id = this.nextId++;
        const payload = JSON.stringify({
            jsonrpc: "2.0",
            id,
            method,
            params,
        }) + "\n";

        if (!this.socket) {
            throw new Error("Intravenous JSON-RPC client is not connected");
        }

        return await new Promise<T>((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.socket!.write(payload, "utf8");
        });
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

            if (message.error) {
                pending.reject(new Error(message.error.message || "unknown JSON-RPC error"));
            } else {
                pending.resolve(message.result);
            }
        }
    }

    private failPending(error: Error): void {
        for (const { reject } of this.pending.values()) {
            reject(error);
        }
        this.pending.clear();
    }

    dispose(): void {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
        this.failPending(new Error("Intravenous client disposed"));
    }
}

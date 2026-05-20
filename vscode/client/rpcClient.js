const net = require("net");

class JsonRpcSocketClient {
    constructor(socketPath, notificationHandler = null) {
        this.socketPath = socketPath;
        this.notificationHandler = notificationHandler;
        this.socket = null;
        this.nextId = 1;
        this.pending = new Map();
        this.buffer = "";
    }

    async connect(timeoutMs = 10000) {
        if (this.socket) {
            return;
        }

        await new Promise((resolve, reject) => {
            const socket = net.createConnection(this.socketPath);
            const timeout = setTimeout(() => {
                socket.destroy();
                reject(new Error(`timed out connecting to ${this.socketPath}`));
            }, timeoutMs);

            socket.on("connect", () => {
                clearTimeout(timeout);
                this.socket = socket;
                socket.setEncoding("utf8");
                socket.on("data", (chunk) => this.onData(chunk));
                socket.on("error", (error) => this.failPending(error));
                socket.on("close", () => this.failPending(new Error("Intravenous server connection closed")));
                resolve();
            });
            socket.on("error", (error) => {
                clearTimeout(timeout);
                reject(error);
            });
        });
    }

    async request(method, params) {
        const id = this.nextId++;
        const payload = JSON.stringify({
            jsonrpc: "2.0",
            id,
            method,
            params,
        }) + "\n";

        return await new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.socket.write(payload, "utf8");
        });
    }

    onData(chunk) {
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

            let message;
            try {
                message = JSON.parse(line);
            } catch (error) {
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

    failPending(error) {
        for (const { reject } of this.pending.values()) {
            reject(error);
        }
        this.pending.clear();
    }

    dispose() {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
        this.failPending(new Error("Intravenous client disposed"));
    }
}

module.exports = { JsonRpcSocketClient };

import { fork, type ChildProcess } from "node:child_process";
import { readFileSync, readlinkSync, realpathSync, statSync } from "node:fs";

export type VSCodeRelaunch = {
    pid: number;
    started: string;
    executable: string;
    argv0: string;
    args: string[];
    cwd: string;
    env: NodeJS.ProcessEnv;
};

export function xWaylandArguments(args: readonly string[]): string[] | null {
    const separator = args.indexOf("--");
    const end = separator === -1 ? args.length : separator;
    const switches = args.slice(0, end);
    if (switches.some((arg) => arg === "--ozone-platform" || arg.startsWith("--ozone-platform="))) return null;
    return [...switches, "--ozone-platform=x11", ...args.slice(end)];
}

export function processStartTime(pid: number): string | null {
    try {
        const stat = readFileSync(`/proc/${pid}/stat`, "utf8");
        // The parenthesized command name can contain spaces and parentheses.
        const fields = stat.slice(stat.lastIndexOf(")") + 2).split(" ");
        return fields[0] === "Z" || fields[0] === "X" ? null : fields[19];
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code === "ENOENT") return null;
        throw error;
    }
}

export function readVSCodeXWaylandRelaunch(pid = Number(process.env.VSCODE_PID)): VSCodeRelaunch | null {
    if (!Number.isSafeInteger(pid) || pid <= 1 || pid === process.pid)
        throw new Error("Cannot identify the local VS Code desktop process");
    const started = processStartTime(pid);
    const path = `/proc/${pid}`;
    const executable = readlinkSync(`${path}/exe`);
    const [argv0, ...originalArgs] = readFileSync(`${path}/cmdline`, "utf8").slice(0, -1).split("\0");
    if (!started || !argv0 || realpathSync(process.execPath) !== executable
        || originalArgs.some((arg) => arg.startsWith("--type=")))
        throw new Error("Cannot verify the local VS Code desktop process");
    const args = xWaylandArguments(originalArgs);
    if (!args) return null;
    const env = Object.fromEntries(readFileSync(`${path}/environ`, "utf8").split("\0").flatMap((entry) => {
        const equals = entry.indexOf("=");
        return equals <= 0 ? [] : [[entry.slice(0, equals), entry.slice(equals + 1)]];
    }));
    if (!env.DISPLAY) throw new Error("The VS Code desktop process has no XWayland display");
    delete env.ELECTRON_RUN_AS_NODE;
    delete env.VSCODE_PID;
    delete env.VSCODE_IPC_HOOK;
    env.INTRAVENOUS_XWAYLAND_RELAUNCH = "1";
    const cwd = readlinkSync(`${path}/cwd`);
    if (!statSync(cwd).isDirectory()) throw new Error("The VS Code working directory is unavailable");
    return { pid, started, executable, argv0, args, cwd, env };
}

export async function prepareVSCodeRelaunch(workerPath: string, plan: VSCodeRelaunch): Promise<ChildProcess | null> {
    const worker = fork(workerPath, [], {
        detached: true, execArgv: [], stdio: ["ignore", "ignore", "ignore", "ipc"],
        env: { ...process.env, ELECTRON_RUN_AS_NODE: "1" },
    });
    try {
        const ready = await new Promise<"ready" | "pending">((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error("The VS Code restart launcher did not respond")), 5000);
            const clear = () => clearTimeout(timeout);
            worker.once("error", (error) => { clear(); reject(error); });
            worker.once("exit", () => { clear(); reject(new Error("The VS Code restart launcher exited before it was ready")); });
            worker.once("message", (message: { state: string; error?: string }) => {
                clear();
                if (message.state === "ready" || message.state === "pending") resolve(message.state);
                else reject(new Error(message.error ?? "The VS Code restart launcher failed"));
            });
            worker.send(plan, (error) => { if (error) { clear(); reject(error); } });
        });
        if (ready === "pending") { if (worker.connected) worker.disconnect(); return null; }
        worker.unref();
        worker.channel?.unref();
        return worker;
    } catch (error) {
        worker.kill();
        throw error;
    }
}

import { fork, type ChildProcess } from "node:child_process";
import { closeSync, mkdirSync, openSync, readFileSync, readlinkSync, realpathSync, statSync } from "node:fs";
import { constants, userInfo } from "node:os";
import { dirname } from "node:path";

export type VSCodeRelaunch = {
    pid: number;
    started: string;
    executable: string;
    argv0: string;
    args: string[];
    cwd: string;
    env: NodeJS.ProcessEnv;
};

function ozonePlatformSwitch(args: readonly string[]): string | undefined {
    const index = args.findIndex((arg) => arg === "--" || arg === "--ozone-platform" || arg.startsWith("--ozone-platform="));
    const arg = args[index];
    if (!arg || arg === "--") return undefined;
    return arg === "--ozone-platform" && args[index + 1] && !args[index + 1].startsWith("--")
        ? `${arg}=${args[index + 1]}` : arg;
}

export function xWaylandArguments(args: readonly string[]): string[] | null {
    if (ozonePlatformSwitch(args)) return null;
    const separator = args.indexOf("--");
    const end = separator === -1 ? args.length : separator;
    const switches = args.slice(0, end);
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

export function requestVSCodeApplicationQuit(plan: Pick<VSCodeRelaunch, "pid" | "started">): void {
    const { pid, started } = plan;
    const status = readFileSync(`/proc/${pid}/status`, "utf8");
    const caught = /^SigCgt:\s+([\da-f]+)$/im.exec(status);
    const sigterm = 1n << BigInt(constants.signals.SIGTERM - 1);
    // Electron routes the first signal through its application quit lifecycle.
    // Chromium restores the default signal handler as soon as it catches it.
    if (!caught || (BigInt(`0x${caught[1]}`) & sigterm) === 0n)
        throw new Error("The editor has no active graceful shutdown handler. Quit VSCodium and start it with --ozone-platform=x11");
    if (processStartTime(pid) !== started) throw new Error("The VS Code desktop process changed before it could quit");
    process.kill(pid, "SIGTERM");
}

export function readVSCodeXWaylandRelaunch(pid = Number(process.env.VSCODE_PID),
    report?: (message: string) => void, waylandSession = true): VSCodeRelaunch | null {
    if (!Number.isSafeInteger(pid) || pid <= 1 || pid === process.pid)
        throw new Error("VSCODE_PID does not identify a separate local VS Code desktop process");
    report?.(`Checking VS Code desktop PID ${pid} from VSCODE_PID.`);
    const started = processStartTime(pid);
    if (!started) throw new Error(`VS Code desktop PID ${pid} has already exited`);
    const path = `/proc/${pid}`;
    const executable = readlinkSync(`${path}/exe`);
    const [argv0, ...originalArgs] = readFileSync(`${path}/cmdline`, "utf8").slice(0, -1).split("\0");
    if (!argv0) throw new Error(`VS Code PID ${pid} has no command line`);
    if (originalArgs.some((arg) => arg.startsWith("--type=")))
        throw new Error(`VS Code PID ${pid} is an Electron subprocess`);
    if (realpathSync(process.execPath) !== executable)
        throw new Error(`VS Code PID ${pid} and the extension host run different executables`);
    const args = xWaylandArguments(originalArgs);
    if (!args) {
        report?.(`Automatic XWayland restart skipped. The desktop process already has ${JSON.stringify(ozonePlatformSwitch(originalArgs))}.`);
        return null;
    }
    if (!waylandSession) {
        report?.("Automatic XWayland restart skipped. No Wayland session was detected and the editor has no explicit --ozone-platform flag.");
        return null;
    }
    const env = Object.fromEntries(readFileSync(`${path}/environ`, "utf8").split("\0").flatMap((entry) => {
        const equals = entry.indexOf("=");
        return equals <= 0 ? [] : [[entry.slice(0, equals), entry.slice(equals + 1)]];
    }));
    if (!env.DISPLAY) {
        if (!process.env.DISPLAY) throw new Error(`Neither VS Code desktop PID ${pid} nor the extension host has DISPLAY for XWayland`);
        env.DISPLAY = process.env.DISPLAY;
        // Keep the host's display and authentication settings together.
        if (process.env.XAUTHORITY) env.XAUTHORITY = process.env.XAUTHORITY;
        else delete env.XAUTHORITY;
        report?.(`VS Code desktop PID ${pid} has no DISPLAY. Using the extension host's XWayland display settings.`);
    } else {
        report?.(`Using XWayland display settings from VS Code desktop PID ${pid}.`);
    }
    // /proc exposes the desktop process's initial environment. Its extension
    // host can have session and home settings that the desktop loaded later.
    const inherited = ["HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
        "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"].filter((key) => {
        const value = process.env[key];
        if (env[key] || !value) return false;
        env[key] = value;
        return true;
    });
    if (inherited.length) report?.(`Restored settings from the extension host: ${inherited.join(", ")}.`);
    if (!env.HOME) {
        env.HOME = userInfo().homedir;
        report?.("Restored HOME from the local user account.");
    }
    delete env.ELECTRON_RUN_AS_NODE;
    delete env.VSCODE_PID;
    delete env.VSCODE_IPC_HOOK;
    env.INTRAVENOUS_XWAYLAND_RELAUNCH = "1";
    const cwd = readlinkSync(`${path}/cwd`);
    if (!statSync(cwd).isDirectory()) throw new Error("The VS Code working directory is unavailable");
    return { pid, started, executable, argv0, args, cwd, env };
}

export async function prepareVSCodeRelaunch(workerPath: string, plan: VSCodeRelaunch,
    diagnostics?: { logFile: string; report: (message: string) => void }): Promise<ChildProcess | null> {
    let logFd: number | undefined;
    if (diagnostics) {
        try {
            mkdirSync(dirname(diagnostics.logFile), { recursive: true });
            logFd = openSync(diagnostics.logFile, "a", 0o600);
            diagnostics.report(`XWayland restart launcher log: ${diagnostics.logFile}`);
        } catch (error) {
            diagnostics.report(`Could not open the XWayland restart launcher log. ${error instanceof Error ? error.message : String(error)}`);
        }
    }
    let worker: ChildProcess;
    try {
        worker = fork(workerPath, [], {
            detached: true, execArgv: [], stdio: ["ignore", "ignore", logFd ?? "ignore", "ipc"],
            env: { ...process.env, ELECTRON_RUN_AS_NODE: "1" },
        });
    } finally {
        if (logFd !== undefined) closeSync(logFd);
    }
    try {
        const ready = await new Promise<"ready" | "pending">((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error("The VS Code restart launcher did not respond")), 5000);
            const clear = () => clearTimeout(timeout);
            worker.once("error", (error) => { clear(); reject(error); });
            worker.once("exit", (code, signal) => {
                clear();
                reject(new Error(`The VS Code restart launcher exited before it was ready (code ${code}, signal ${signal})`));
            });
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

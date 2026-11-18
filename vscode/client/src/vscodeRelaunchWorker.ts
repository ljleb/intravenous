import { spawn } from "node:child_process";
import { createServer } from "node:net";
import { processStartTime, type VSCodeRelaunch } from "./vscodeRelaunch";

process.once("message", (plan: VSCodeRelaunch) => {
    // Several windows can activate the extension at once. Only one launcher
    // may wait for this instance, and the kernel releases the address on exit.
    const lock = createServer((socket) => socket.destroy());
    let polling: ReturnType<typeof setInterval> | undefined;
    const deadline = Date.now() + 120_000;
    const finish = () => {
        clearInterval(polling);
        lock.close();
        if (process.connected) process.disconnect?.();
    };
    lock.once("error", (error: NodeJS.ErrnoException) => {
        if (process.connected) process.send?.({ state: error.code === "EADDRINUSE" ? "pending" : "failed", error: error.message }, finish);
        else finish();
    });
    lock.listen(`\0intravenous-vscode-relaunch-${process.getuid?.()}-${plan.pid}-${plan.started}`, () => {
        if (!process.connected || processStartTime(plan.pid) !== plan.started) { finish(); return; }
        process.send?.({ state: "ready" });
        polling = setInterval(() => {
            if (Date.now() >= deadline) { finish(); return; }
            if (processStartTime(plan.pid) === plan.started) return;
            const editor = spawn(plan.executable, plan.args, {
                argv0: plan.argv0, cwd: plan.cwd, env: plan.env, detached: true, stdio: "ignore",
            });
            editor.once("error", () => { process.exitCode = 1; });
            editor.unref();
            finish();
        }, 100);
    });
});

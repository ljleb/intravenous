import { spawn } from "node:child_process";
import { createServer } from "node:net";
import { processStartTime, type VSCodeRelaunch } from "./vscodeRelaunch";

function report(message: string): void { console.error(`[${new Date().toISOString()}] ${message}`); }

process.once("message", (plan: VSCodeRelaunch) => {
    report(`Launcher PID ${process.pid} received the restart request for editor PID ${plan.pid}.`);
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
        report(error.code === "EADDRINUSE" ? "Another launcher already owns this restart." : `Could not claim the restart. ${error.message}`);
        if (process.connected) process.send?.({ state: error.code === "EADDRINUSE" ? "pending" : "failed", error: error.message }, finish);
        else finish();
    });
    lock.listen(`\0intravenous-vscode-relaunch-${process.getuid?.()}-${plan.pid}-${plan.started}`, () => {
        if (!process.connected) { report("The extension host disconnected before the launcher was ready."); finish(); return; }
        if (processStartTime(plan.pid) !== plan.started) {
            const error = `Editor PID ${plan.pid} exited before the launcher was ready.`;
            report(error);
            process.send?.({ state: "failed", error }, finish);
            return;
        }
        report(`Ready. Waiting up to two minutes for editor PID ${plan.pid} to exit.`);
        process.send?.({ state: "ready" });
        polling = setInterval(() => {
            if (processStartTime(plan.pid) === plan.started) {
                if (Date.now() >= deadline) {
                    report(`Timed out. Editor PID ${plan.pid} is still running.`);
                    finish();
                }
                return;
            }
            report(`Editor PID ${plan.pid} exited. Starting the replacement with --ozone-platform=x11.`);
            const editor = spawn(plan.executable, plan.args, {
                argv0: plan.argv0, cwd: plan.cwd, env: plan.env, detached: true, stdio: "ignore",
            });
            editor.once("spawn", () => report(`Started the replacement process with PID ${editor.pid}.`));
            editor.once("error", (error) => { report(`Could not start the replacement process. ${error.message}`); process.exitCode = 1; });
            editor.unref();
            finish();
        }, 100);
    });
});

import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir, userInfo } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { prepareVSCodeRelaunch, processStartTime, readVSCodeXWaylandRelaunch, xWaylandArguments } from "../src/vscodeRelaunch";

test("XWayland restart preserves editor arguments and respects an explicit graphics backend", () => {
    assert.deepEqual(xWaylandArguments(["--user-data-dir=/tmp/editor profile", "--", "--ozone-platform.txt"]),
        ["--user-data-dir=/tmp/editor profile", "--ozone-platform=x11", "--", "--ozone-platform.txt"]);
    assert.deepEqual(xWaylandArguments([]), ["--ozone-platform=x11"]);
    for (const backend of ["x11", "wayland", "auto"]) {
        assert.equal(xWaylandArguments([`--ozone-platform=${backend}`]), null);
        assert.equal(xWaylandArguments(["--ozone-platform", backend]), null);
    }
});

test("a launcher can distinguish a running Linux process from a missing one", { skip: process.platform !== "linux" }, () => {
    const started = processStartTime(process.pid);
    assert.match(started!, /^\d+$/);
    assert.equal(processStartTime(process.pid), started);
    assert.equal(processStartTime(2 ** 31 - 1), null);
});

test("an explicit X11 flag explains the restart skip even without Wayland environment variables",
    { skip: process.platform !== "linux" }, async () => {
        const desktop = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)", "editor", "--ozone-platform=x11"],
            { stdio: "ignore" });
        try {
            await once(desktop, "spawn");
            const log: string[] = [];
            assert.equal(readVSCodeXWaylandRelaunch(desktop.pid, (line) => log.push(line), false), null);
            assert.match(log.at(-1)!, /already has "--ozone-platform=x11"/);
        } finally {
            desktop.kill();
        }
    });

test("XWayland restart fills missing settings from the extension host while keeping desktop settings",
    { skip: process.platform !== "linux" }, async () => {
        const desktop = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
            env: { ...process.env, DISPLAY: undefined, XAUTHORITY: "/tmp/editor.xauth", HOME: undefined,
                XDG_CACHE_HOME: "/tmp/editor-cache", XDG_SESSION_TYPE: undefined }, stdio: "ignore",
        });
        const saved = Object.fromEntries(["DISPLAY", "XAUTHORITY", "HOME", "XDG_CACHE_HOME", "XDG_SESSION_TYPE"]
            .map((key) => [key, process.env[key]]));
        try {
            await once(desktop, "spawn");
            delete process.env.DISPLAY;
            assert.throws(() => readVSCodeXWaylandRelaunch(desktop.pid), /Neither .* nor the extension host has DISPLAY/);

            process.env.DISPLAY = ":23";
            process.env.XAUTHORITY = "/tmp/extension.xauth";
            process.env.HOME = "/tmp/extension-home";
            process.env.XDG_CACHE_HOME = "/tmp/extension-cache";
            process.env.XDG_SESSION_TYPE = "wayland";
            const log: string[] = [];
            const plan = readVSCodeXWaylandRelaunch(desktop.pid, (line) => log.push(line));
            assert.ok(plan?.args.includes("--ozone-platform=x11"));
            assert.equal(plan.env.DISPLAY, ":23");
            assert.equal(plan.env.XAUTHORITY, "/tmp/extension.xauth");
            assert.equal(plan.env.HOME, "/tmp/extension-home");
            assert.equal(plan.env.XDG_CACHE_HOME, "/tmp/editor-cache");
            assert.equal(plan.env.XDG_SESSION_TYPE, "wayland");
            assert.ok(log.some((line) => line.includes("Using the extension host's XWayland display settings")));

            delete process.env.XAUTHORITY;
            delete process.env.HOME;
            const accountFallback = readVSCodeXWaylandRelaunch(desktop.pid);
            assert.equal(accountFallback?.env.XAUTHORITY, undefined);
            assert.equal(accountFallback?.env.HOME, userInfo().homedir);
        } finally {
            for (const [key, value] of Object.entries(saved)) {
                if (value === undefined) delete process.env[key];
                else process.env[key] = value;
            }
            desktop.kill();
        }
    });

test("the XWayland launcher records the editor exit and replacement after the host lets it run",
    { skip: process.platform !== "linux", timeout: 10_000 }, async () => {
        const directory = await mkdtemp(join(tmpdir(), "iv-xwayland-restart-"));
        const logFile = join(directory, "logs", "xwayland-restart.log");
        const original = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], { stdio: "ignore" });
        let launcher: Awaited<ReturnType<typeof prepareVSCodeRelaunch>> = null;
        try {
            await once(original, "spawn");
            const pid = original.pid!;
            const started = processStartTime(pid)!;
            const reports: string[] = [];
            launcher = await prepareVSCodeRelaunch(require.resolve("../src/vscodeRelaunchWorker"), {
                pid, started, executable: process.execPath, argv0: process.execPath,
                args: ["-e", ""], cwd: directory, env: process.env,
            }, { logFile, report: (line) => reports.push(line) });
            assert.ok(launcher);
            assert.ok(reports.some((line) => line.includes(logFile)));
            const whileRunning = await readFile(logFile, "utf8");
            assert.ok(whileRunning.includes(`Waiting up to two minutes for editor PID ${pid} to exit.`));
            assert.doesNotMatch(whileRunning, /Started the replacement process/);

            launcher.ref();
            const exited = once(launcher, "exit");
            const originalExited = once(original, "exit");
            original.kill();
            await originalExited;
            assert.deepEqual(await exited, [0, null]);
            const completed = await readFile(logFile, "utf8");
            assert.ok(completed.includes(`Editor PID ${pid} exited.`));
            assert.match(completed, /Started the replacement process with PID \d+/);
        } finally {
            original.kill();
            launcher?.kill();
            await rm(directory, { recursive: true, force: true });
        }
    });

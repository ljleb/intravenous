import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter, once } from "node:events";
import { spawn, type ChildProcess } from "node:child_process";
import { PassThrough } from "node:stream";
import { NativeClientHelper } from "../src/nativeClientHelper";
import type { CursorWarp } from "../src/graphEditorProtocol";

function fakeChild() {
    const events = new EventEmitter();
    const connection = new PassThrough();
    const messages: Array<{ method: string; params: unknown }> = [];
    const signals: string[] = [];
    connection.on("data", (bytes) => { messages.push(JSON.parse(String(bytes))); });
    const child = Object.assign(events, {
        stdio: [null, null, null, connection],
        kill: (signal: string) => { signals.push(signal); return true; },
    }) as unknown as ChildProcess;
    return { child, connection, messages, signals,
        ready: (cursorWarp = true, cursorWarpUnavailableReason?: string) => connection.write(JSON.stringify({
            method: "native.ready", params: { cursorWarp, cursorWarpUnavailableReason },
        }) + "\n"),
        crash: () => events.emit("close", 1, null) };
}

const cursorWarp = (): CursorWarp => ({ originX: 100, originY: 100, deltaX: 20, deltaY: -20, expiresAt: Date.now() + 100 });
const warpMessages = (run: ReturnType<typeof fakeChild>) => run.messages.filter(({ method }) => method === "native.warpCursor");

test("the helper starts eagerly and drops cursor warps across a crash and restart", (t) => {
    t.mock.timers.enable({ apis: ["setTimeout", "Date"], now: 1000 });
    const children: Array<ReturnType<typeof fakeChild>> = [];
    const helper = new NativeClientHelper(() => {
        const run = fakeChild();
        children.push(run);
        return run.child;
    }, () => {});
    t.after(() => helper.dispose());
    helper.start();
    helper.start();
    assert.equal(children.length, 1);
    helper.warpCursor(cursorWarp());
    const first = children[0];
    assert.deepEqual(warpMessages(first), []);
    first.ready();
    const warp = cursorWarp();
    helper.warpCursor(warp);
    assert.deepEqual(warpMessages(first).map(({ params }) => params), [warp]);
    first.crash();
    helper.warpCursor(cursorWarp());
    t.mock.timers.tick(999);
    assert.equal(children.length, 1);
    t.mock.timers.tick(1);
    const second = children[1];
    second.ready();
    assert.deepEqual(warpMessages(second), []);
    helper.warpCursor(warp);
    assert.deepEqual(warpMessages(second), []);
    helper.warpCursor(cursorWarp());
    assert.equal(warpMessages(second).length, 1);
});

test("the helper backs off across repeated exits and stops retrying on disposal", (t) => {
    t.mock.timers.enable({ apis: ["setTimeout", "Date"], now: 1000 });
    const children: Array<ReturnType<typeof fakeChild>> = [];
    const helper = new NativeClientHelper(() => {
        const run = fakeChild();
        children.push(run);
        return run.child;
    }, () => {});
    t.after(() => helper.dispose());
    helper.start();
    children[0].crash();
    t.mock.timers.tick(1000);
    children[1].crash();
    t.mock.timers.tick(1999);
    assert.equal(children.length, 2);
    t.mock.timers.tick(1);
    const current = children[2];
    current.ready(false);
    helper.warpCursor(cursorWarp());
    assert.deepEqual(warpMessages(current), []);
    helper.dispose();
    assert.equal(current.connection.destroyed, true);
    assert.deepEqual(current.signals, ["SIGTERM"]);
    current.crash();
    t.mock.timers.tick(60000);
    helper.start();
    assert.equal(children.length, 3);
});

test("unavailable cursor return logs the native capability reason once per launch", (t) => {
    const run = fakeChild();
    const messages: string[] = [];
    const helper = new NativeClientHelper(() => run.child, (message) => messages.push(message));
    t.after(() => helper.dispose());
    helper.start();
    run.ready(false, "The native helper was built without X11 support.");
    run.ready(false);
    helper.warpCursor(cursorWarp());
    assert.deepEqual(messages, ["Automation cursor return is unavailable. The native helper was built without X11 support."]);
    assert.deepEqual(warpMessages(run), []);
});

test("cursor return traces readiness and preserves the trace flag on the native request", (t) => {
    t.mock.timers.enable({ apis: ["Date"], now: 1000 });
    const run = fakeChild();
    const messages: string[] = [];
    const helper = new NativeClientHelper(() => run.child, (message) => messages.push(message));
    t.after(() => helper.dispose());
    helper.start();
    const request = { ...cursorWarp(), trace: true };
    helper.warpCursor(request);
    assert.match(messages[0], /not ready/);
    run.ready();
    helper.warpCursor(request);
    assert.match(messages[1], /dispatched/);
    assert.deepEqual(warpMessages(run).map(({ params }) => params), [request]);
});

test("disposal closes the inherited connection so the child can exit on EOF", { timeout: 3000 }, async (t) => {
    let child!: ChildProcess;
    const helper = new NativeClientHelper(() => {
        child = spawn(process.execPath, ["-e", `
            const fs = require("node:fs");
            process.on("SIGTERM", () => {});
            fs.writeSync(3, JSON.stringify({ method: "native.ready", params: {} }) + "\\n");
            const connection = fs.createReadStream(null, { fd: 3 });
            connection.on("end", () => process.exit(0));
            connection.resume();
        `], { stdio: ["ignore", "ignore", "ignore", "pipe"] });
        return child;
    }, () => {});
    t.after(() => {
        helper.dispose();
        if (child && child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
    });
    helper.start();
    const closed = once(child, "close");
    await Promise.race([once(child.stdio[3]!, "data"), closed.then(([code]) => {
        throw new Error(`Helper exited before readiness: ${code}`);
    })]);
    helper.dispose();
    assert.deepEqual(await closed, [0, null]);
});

import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter, once } from "node:events";
import { spawn, type ChildProcess } from "node:child_process";
import { Duplex } from "node:stream";
import { NativeClientHelper } from "../src/nativeClientHelper";
import type { CursorWarp } from "../src/graphEditorProtocol";

function fakeChild() {
    const events = new EventEmitter();
    const messages: Array<{ method: string; params: unknown; id?: number }> = [];
    const signals: string[] = [];
    const connection = new Duplex({
        read() {},
        write(bytes, _encoding, callback) {
            messages.push(JSON.parse(String(bytes)));
            callback();
        },
    });
    const receive = (message: object) => connection.emit("data", JSON.stringify(message) + "\n");
    const child = Object.assign(events, {
        stdio: [null, null, null, connection],
        kill: (signal: string) => { signals.push(signal); return true; },
    }) as unknown as ChildProcess;
    return { child, connection, messages, signals, receive,
        ready: (cursorWarp = true, cursorWarpUnavailableReason?: string, cursorWarpResult = false) => receive({
            method: "native.ready", params: { cursorWarp, cursorWarpUnavailableReason, cursorWarpResult },
        }),
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

test("cursor return preserves both coordinate pairs and can request a hidden cursor", (t) => {
    t.mock.timers.enable({ apis: ["Date"], now: 1000 });
    const run = fakeChild();
    const messages: string[] = [];
    const helper = new NativeClientHelper(() => run.child, (message) => messages.push(message));
    t.after(() => helper.dispose());
    helper.start();
    const request = { ...cursorWarp(), originX: 7055, originY: 913, screenOrigin: { x: 4899, y: 634 } };
    helper.warpCursor(request);
    assert.match(messages[0], /not ready/);
    run.ready();
    helper.warpCursor(request);
    helper.warpCursor({ ...request, screenOrigin: { x: 4899.5, y: 634 } });
    assert.match(messages[1], /coordinates were invalid/);
    helper.warpCursor({ ...request, expiresAt: Date.now() });
    assert.match(messages[2], /^Cursor return skipped.*expired/);
    helper.warpCursor(request, true);
    assert.deepEqual(warpMessages(run).map(({ params }) => params), [request, { ...request, hideCursor: true }]);
});

test("cursor return only confirms a move accepted by the helper before expiry", async (t) => {
    t.mock.timers.enable({ apis: ["Date"], now: 1000 });
    const run = fakeChild();
    const helper = new NativeClientHelper(() => run.child, () => {});
    t.after(() => helper.dispose());
    helper.start();
    run.ready(true, undefined, true);
    let completed = 0;
    const confirm = () => { ++completed; };
    const reply = async (moved: boolean) => {
        const id = warpMessages(run).at(-1)?.id;
        assert.equal(typeof id, "number");
        run.receive({ id, result: { moved } });
        await new Promise<void>((resolve) => setImmediate(resolve));
    };

    helper.warpCursor(cursorWarp(), true, confirm);
    await reply(false);
    assert.equal(completed, 0);
    helper.warpCursor(cursorWarp(), true, confirm);
    await reply(true);
    assert.equal(completed, 1);
    helper.warpCursor(cursorWarp(), true, confirm);
    t.mock.timers.tick(101);
    await reply(true);
    assert.equal(completed, 1);
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

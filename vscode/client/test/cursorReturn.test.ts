import test from "node:test";
import assert from "node:assert/strict";
import type { CursorReturnMessage, CursorReturnPolicy } from "../src/graphEditorProtocol";
import { PointerLockExitGuard } from "../src/pointerLockExitGuard";
import { timelineSampleFraction } from "../src/timelineGeometry";
import { timelineSampleAt, timelineSampleCount, timelineSamples } from "../src/timelineSamples";
import { cursorWarpAnchor, PointerLockCursorReturn, visibleCursorDestination } from "../webview/graph/cursorReturn";

const origin = { x: 10, y: 20 };
const anchor = cursorWarpAnchor({ pointerType: "mouse", screenX: 100, screenY: 200 }, 1.5)!;
const destination = () => ({ x: 40, y: 16 });
function harness() {
    let locked = true;
    const guard = new PointerLockExitGuard();
    const render: (() => void)[] = [];
    const messages: CursorReturnMessage[] = [];
    const cursor = new PointerLockCursorReturn(guard, () => locked, (run) => render.push(run), (message) => messages.push(message));
    return { guard, render, messages, cursor, unlock() { locked = false; cursor.unlocked(); } };
}

test("object and wrapped drags share one native request after unlock and render", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const policies: (CursorReturnPolicy | undefined)[] = [undefined, "presentationPoint", "playhead", "wrapped"];
    for (const policy of policies) {
        const { cursor, guard, unlock, render, messages } = harness();
        cursor.finish(origin, origin, { ...anchor, destination, policy });
        cursor.unlocked();
        assert.deepEqual(render, []);
        assert.deepEqual(guard.origin(), origin);

        unlock();
        assert.deepEqual(messages, []);
        render.shift()!();
        assert.deepEqual(messages, [{ type: "returnCursor", requestId: 1, policy,
            warp: { originX: 150, originY: 300, screenOrigin: { x: 100, y: 200 },
                deltaX: 45, deltaY: -6, expiresAt: 1100 } }]);
        cursor.complete(2);
        assert.deepEqual(guard.origin(), origin);
        cursor.complete(1);
        assert.equal(guard.origin(), null);
    }
});

test("a new press or an expired unlock cannot move the native cursor", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, render, unlock, messages } = harness();
    cursor.finish(origin, origin, { ...anchor, destination });
    unlock();
    cursor.cancel("A new pointer press arrived.");
    render.shift()!();
    assert.deepEqual(messages, [{ type: "cursorReturnSkipped", detail: "A new pointer press arrived." }]);
    cursor.finish(origin, origin, { ...anchor, destination });
    t.mock.timers.tick(100);
    cursor.unlocked();
    assert.equal(cursor.waitingForUnlock, false);
    assert.equal(render.length, 0);
    assert.deepEqual(messages.at(-1), { type: "cursorReturnSkipped",
        detail: "The browser did not finish releasing pointer lock in time." });
});

test("cursor return waits for the release frame and ignores replies that arrive after its deadline", async (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, render, unlock, messages } = harness();
    let reply!: (point: ReturnType<typeof destination>) => void;
    const delayed = () => new Promise<ReturnType<typeof destination>>((resolve) => { reply = resolve; });

    cursor.finish(origin, origin, { ...anchor, destination: delayed, policy: "presentationPoint" });
    unlock();
    cursor.unlocked();
    assert.equal(render.length, 1);
    render.shift()!();
    assert.deepEqual(messages, []);
    reply(destination());
    await Promise.resolve();
    assert.equal(messages[0].type, "returnCursor");

    cursor.finish(origin, origin, { ...anchor, destination: delayed, policy: "presentationPoint" });
    cursor.unlocked();
    render.shift()!();
    t.mock.timers.tick(100);
    reply(destination());
    await Promise.resolve();
    assert.equal(messages.length, 2);
    assert.equal(messages[1].type, "cursorReturnSkipped");
});

test("playhead tracing follows one cursor return through unlock and native confirmation", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, messages, render, unlock } = harness();
    const steps: string[] = [];
    cursor.finish(origin, origin, { ...anchor, destination, policy: "playhead" }, "",
        { id: "playhead-1", log: (step) => steps.push(step) });
    unlock();
    render.shift()!();
    assert.equal(messages[0].type, "returnCursor");
    if (messages[0].type === "returnCursor") {
        assert.equal(messages[0].traceId, "playhead-1");
        assert.equal(messages[0].policy, "playhead");
    }
    cursor.complete(1);
    assert.deepEqual(steps, ["Finished pointer session", "Browser released pointer lock",
        "Sending cursor return to the extension", "Webview received native move confirmation"]);
});

test("capture fallback, canceled drags and non-mouse input never submit a cursor return", () => {
    const { cursor, messages } = harness();
    cursor.finish(origin, null, { ...anchor, destination }, "Pointer lock denied");
    cursor.finish(null, origin, { ...anchor, destination });
    assert.equal(cursorWarpAnchor({ pointerType: "touch", screenX: 100, screenY: 200 }, 1.5), null);
    assert.equal(cursor.waitingForUnlock, false);
    assert.deepEqual(messages, [{ type: "cursorReturnSkipped", detail: "Pointer lock denied" },
        { type: "cursorReturnSkipped", detail: "The drag ended before a normal pointer-lock release." }]);
});

test("playheads and presentation points can land under controls within the visible surface", (t) => {
    const heading = {
        contains: (node: unknown) => node === ruler,
        getBoundingClientRect: () => ({ bottom: 30 }),
    };
    let viewportBottom = 100;
    const timeline = {
        getBoundingClientRect: () => ({ left: 0, right: 200, top: 0, bottom: viewportBottom }),
        querySelector: () => heading,
    };
    const ruler = {
        getBoundingClientRect: () => ({ left: 20, right: 120, top: 10, bottom: 30 }),
        closest: () => timeline,
        contains: () => false,
    } as unknown as Element;
    const surface = {
        getBoundingClientRect: () => ({ left: 20, right: 120, top: 25, bottom: 110 }),
        closest: () => timeline,
        contains: () => false,
    } as unknown as Element;
    const window = Object.getOwnPropertyDescriptor(globalThis, "window");
    const document = Object.getOwnPropertyDescriptor(globalThis, "document");
    Object.defineProperty(globalThis, "window", { configurable: true, value: { innerWidth: 200, innerHeight: 100 } });
    const overlay = { getBoundingClientRect: () => ({ left: 104, right: 120 }) };
    Object.defineProperty(globalThis, "document", { configurable: true, value: { elementFromPoint: () => overlay } });
    t.after(() => {
        if (window) Object.defineProperty(globalThis, "window", window);
        else Reflect.deleteProperty(globalThis, "window");
        if (document) Object.defineProperty(globalThis, "document", document);
        else Reflect.deleteProperty(globalThis, "document");
    });
    const fullLap = { beginSample: "0", beginFraction: 0, spanSamples: Number(timelineSampleCount) };
    const wrappedSample = timelineSampleAt(timelineSamples(fullLap), -0.0625);
    const wrappedTarget = { x: 20 + 100 * timelineSampleFraction(wrappedSample, fullLap), y: 21 };
    assert.deepEqual(wrappedTarget, { x: 113.75, y: 21 });
    assert.deepEqual(visibleCursorDestination(ruler, wrappedTarget), wrappedTarget);
    assert.deepEqual(visibleCursorDestination(ruler, { x: 120, y: 21 }), { x: 118, y: 21 });
    assert.deepEqual(visibleCursorDestination(surface, { x: 20.5, y: 45 }), { x: 20.5, y: 45 });
    assert.deepEqual(visibleCursorDestination(surface, { x: 110, y: 45 }), { x: 110, y: 45 });
    assert.deepEqual(visibleCursorDestination(surface, { x: 180, y: 45 }), { x: 118, y: 45 });
    assert.deepEqual(visibleCursorDestination(surface, { x: 60, y: 27 }), { x: 60, y: 32 });
    assert.deepEqual(visibleCursorDestination(surface, { x: 60, y: 110 }), { x: 60, y: 98 });
    viewportBottom = 25;
    assert.equal(visibleCursorDestination(surface, { x: 60, y: 45 }), null);
});

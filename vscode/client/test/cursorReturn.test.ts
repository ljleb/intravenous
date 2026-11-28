import test from "node:test";
import assert from "node:assert/strict";
import type { CursorReturnMessage, CursorReturnPolicy } from "../src/graphEditorProtocol";
import type { Point } from "../src/graphScope";
import { PointerLockExitGuard } from "../src/pointerLockExitGuard";
import type { PresentationInput, PresentationSnapshot } from "../src/presentationPlugin";
import { PresentationPluginSession } from "../src/presentationPluginSession";
import { timelineSampleFraction } from "../src/timelineGeometry";
import { timelineSampleAt, timelineSampleCount, timelineSamples } from "../src/timelineSamples";
import { cursorWarpAnchor, cursorWindowReference, PointerLockCursorReturn, presentationCursorPoint,
    visibleCursorDestination } from "../webview/graph/cursorReturn";
import { presentationDrag } from "../webview/presentation/presentationDrag";

const origin = { x: 10, y: 20 };
const anchor = cursorWarpAnchor({ pointerType: "mouse", screenX: 100, screenY: 200 }, 1.5)!;
const destination = () => ({ x: 40, y: 16 });

test("a presentation cursor reply follows an exact sample across the signed seam and a viewport change", () => {
    const landing = { sample: String(-(1n << 63n) + 3n), yFraction: .25 };
    const bounds = { left: 20, top: 30, width: 100, height: 200 };
    const time = { beginSample: String((1n << 63n) - 2n), beginFraction: 0, spanSamples: 10 };
    assert.deepEqual(presentationCursorPoint(landing, bounds, time), { x: 70, y: 80 });
    assert.deepEqual(presentationCursorPoint(landing, { ...bounds, width: 160 },
        { ...time, beginSample: String(-(1n << 63n)), spanSamples: 6 }), { x: 100, y: 80 });
});

function harness(wayland = false) {
    let locked = !wayland;
    const guard = new PointerLockExitGuard();
    const render: (() => void)[] = [];
    const messages: CursorReturnMessage[] = [];
    const cursor = new PointerLockCursorReturn(guard, () => locked, (run) => render.push(run), (message) => messages.push(message));
    cursor.available = wayland;
    return { guard, render, messages, cursor, unlock() { locked = false; cursor.unlocked(); } };
}

test("object and wrapped drags share one native request after unlock and render", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const policies: (CursorReturnPolicy | undefined)[] = [undefined, "playhead", "wrapped"];
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

test("a point return uses the release event's window mapping and the final CSS position", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, render, unlock, messages } = harness();
    const windowReference = cursorWindowReference({ screenX: 4908, screenY: 407, clientX: 312, clientY: 200 },
        { screenX: 3840, screenY: 0, outerWidth: 1920, outerHeight: 1080, devicePixelRatio: 1.44 });
    cursor.finish(origin, origin, { windowReference, policy: "presentationPoint", destination: () => ({ x: 362, y: 225 }) });
    unlock();
    render.shift()!();
    assert.deepEqual(messages, [{ type: "returnCursor", requestId: 1, policy: "presentationPoint", warp: {
        destination: { windowSizeDip: { width: 1920, height: 1080 }, windowPointDip: { x: 1068, y: 407 },
            offsetPixels: { x: 72, y: 36 } }, expiresAt: 1100,
    } }]);
});

const waylandPress = { pointerType: "mouse", isTrusted: true, screenX: 443, screenY: 313, clientX: 10, clientY: 20,
    button: 0, timeStamp: 25 };
const waylandWindow = () => ({ screenX: 400, screenY: 250, outerWidth: 1000, outerHeight: 500, devicePixelRatio: 1.5 });
const modifiers = { shiftKey: false, altKey: false, ctrlKey: false, metaKey: false };
const ignore = () => {};

test("Wayland returns through the trusted press mapping after Mutter releases", (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, guard, render, messages } = harness(true);
    const input = { sample: ignore, cancel: ignore, denied: ignore,
        release: () => cursor.finish(origin, origin, { ...anchor, destination, policy: "wrapped" }) };
    assert.equal(cursor.beginWayland({ ...waylandPress, isTrusted: false }, waylandWindow(), 975, input), false);
    assert.deepEqual(messages, []);
    assert.equal(cursor.beginWayland(waylandPress, waylandWindow(), 975, input), true);
    const begin = messages[0];
    assert.ok(begin.type === "beginWaylandPointer");
    assert.equal(begin.press.time, 1000);
    assert.equal(cursor.nativeGranted, false);
    cursor.receiveWayland({ gesture: begin.press.gesture, granted: true });
    assert.equal(cursor.nativeGranted, true);
    cursor.receiveWayland({ gesture: begin.press.gesture, end: { kind: "release", modifiers } });
    assert.equal(cursor.nativeDragging, false);
    assert.equal(guard.origin(), null);
    assert.equal(messages.length, 1);
    render.shift()!();
    assert.deepEqual(messages[1], { type: "returnCursor", requestId: 1, policy: "wrapped", warp: {
        destination: { windowSizeDip: { width: 1000, height: 500 }, windowPointDip: { x: 43, y: 63 },
            offsetPixels: { x: 45, y: -6 } },
        expiresAt: 1100, waylandGesture: begin.press.gesture,
    } });
});

test("a trusted regrab waits for the old release and cancels the old destination", () => {
    const { cursor, render, messages } = harness(true);
    const order: string[] = [];
    const input = { sample: () => { order.push("move"); }, cancel: ignore, denied: ignore,
        release: () => {
            order.push("old release");
            cursor.finish(origin, origin, { ...anchor, destination });
        } };
    cursor.beginWayland(waylandPress, waylandWindow(), 0, input);
    const begin = messages[0];
    assert.ok(begin.type === "beginWaylandPointer");
    const next = { ...waylandPress, target: null, pointerId: 1, preventDefault: ignore, stopPropagation: ignore } as unknown as PointerEvent;
    assert.equal(cursor.deferPress(next, () => {
        order.push("new press");
        assert.equal(cursor.releasedBeforeStart, next);
        assert.equal(cursor.beginWayland(waylandPress, waylandWindow(), 0, input), false);
    }), true);
    assert.equal(cursor.notePointerUp(next), true);
    assert.deepEqual(order, []);
    cursor.receiveWayland({ gesture: begin.press.gesture, granted: true,
        samples: [{ kind: "motion", x: 5, y: -1, modifiers }], end: { kind: "release", modifiers } });
    assert.deepEqual(order, ["move", "old release", "new press"]);
    assert.equal(render.length, 0);
    assert.ok(messages.some((message) => message.type === "cancelWaylandCursor" && message.gesture === begin.press.gesture));
    assert.equal(messages.some(({ type }) => type === "returnCursor"), false);
});

test("Wayland disarms a return when the browser window scale changes", () => {
    const { cursor, render, messages } = harness(true);
    const view = waylandWindow();
    cursor.beginWayland(waylandPress, view, 0, { sample: ignore, cancel: ignore, denied: ignore,
        release: () => cursor.finish(origin, origin, { ...anchor, destination }) });
    const begin = messages[0];
    assert.ok(begin.type === "beginWaylandPointer");
    cursor.receiveWayland({ gesture: begin.press.gesture, granted: true, end: { kind: "release", modifiers } });
    view.devicePixelRatio = 2;
    render.shift()!();
    assert.deepEqual(messages.slice(-2), [
        { type: "cancelWaylandCursor", gesture: begin.press.gesture },
        { type: "cursorReturnSkipped", detail: "The window geometry changed during the drag." },
    ]);
    assert.equal(messages.some(({ type }) => type === "returnCursor"), false);
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

test("playhead return waits for its destination and ignores replies after its deadline", async (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    const { cursor, render, unlock, messages } = harness();
    let reply!: (point: ReturnType<typeof destination>) => void;
    const delayed = () => new Promise<ReturnType<typeof destination>>((resolve) => { reply = resolve; });

    cursor.finish(origin, origin, { ...anchor, destination: delayed, policy: "playhead" });
    unlock();
    cursor.unlocked();
    assert.equal(render.length, 1);
    render.shift()!();
    assert.deepEqual(messages, []);
    reply(destination());
    await Promise.resolve();
    assert.equal(messages[0].type, "returnCursor");

    cursor.finish(origin, origin, { ...anchor, destination: delayed, policy: "playhead" });
    cursor.unlocked();
    render.shift()!();
    t.mock.timers.tick(100);
    reply(destination());
    await Promise.resolve();
    assert.equal(messages.length, 2);
    assert.equal(messages[1].type, "cursorReturnSkipped");
});

test("fast point drags fuzz release, unlock, render, worker reply, and interruption timing", async (t) => {
    t.mock.timers.enable({ apis: ["Date", "setTimeout"], now: 1000 });
    let seed = 50712;
    const pick = (limit: number) => {
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        return Math.floor(seed / 2 ** 32 * limit);
    };
    const snapshot: PresentationSnapshot = {
        node: { definitionId: "test.curve", arguments: [], ports: [] }, revision: 1, mode: "editable", editable: true,
        viewport: { width: 600, height: 100, time: { beginSample: "0", beginFraction: 0, spanSamples: 48000 } },
        observations: {}, state: null,
    };
    const windowReference = { windowSizeDip: { width: 1000, height: 500 }, windowPointDip: { x: 200, y: 180 },
        clientPoint: origin, devicePixelRatio: 1.25 };
    for (let iteration = 0; iteration < 256; ++iteration) {
        const { cursor, unlock, guard, render, messages } = harness();
        let sendWorkerReply!: () => void;
        const worker = new Promise<void>((resolve) => { sendWorkerReply = resolve; });
        let completeCursor!: (point: Point | null) => void;
        const released = new Promise<Point | null>((resolve) => { completeCursor = resolve; });
        let finish: PresentationInput | undefined;
        const session = new PresentationPluginSession(async (_view, inputs) => {
            const last = inputs.at(-1);
            assert.ok(last?.type === "gesture" && last.phase === "finish");
            assert.deepEqual(last.point, expected);
            assert.deepEqual(last.viewport, snapshot.viewport);
            await worker;
            return { drag: { cursor: { x: Math.abs(last.point.x) % 600, y: Math.abs(last.point.y) % 100 } } };
        }, () => {}, (frame, _, inputs) => {
            const point = frame.drag?.cursor;
            if (finish && inputs.includes(finish)) completeCursor(point && "x" in point ? point : null);
        }, (error) => { throw error; });
        session.update(snapshot);
        const drag = presentationDrag({ clientX: origin.x, clientY: origin.y } as PointerEvent,
            { left: 0, top: 0 }, { id: "point" }, (input) => {
                const queued = session.input(input);
                if (input.phase === "finish") finish = queued;
            }, { cursorReturn: () => ({ policy: "presentationPoint", destination: () => released }) });
        drag.begin?.();
        const expected = { ...origin };
        for (let remaining = 10 + pick(420); remaining > 0; --remaining) {
            const delta = { x: pick(2001) - 1000, y: pick(2001) - 1000 };
            drag.moveBy(delta);
            expected.x += delta.x;
            expected.y += delta.y;
        }
        const follow = drag.finish(origin, origin);
        assert.ok(follow);
        cursor.finish(origin, origin, { ...follow, windowReference });
        const flush = session.flush();
        const releaseAt = Date.now();
        const unlockAt = pick(160);
        const renderAt = unlockAt + 1 + pick(180);
        const workerAt = pick(400);
        const readyAt = Math.max(renderAt, workerAt);
        const cancelReason = pick(4) === 0 ? "A new pointer press arrived." : undefined;
        const events = [
            { time: unlockAt, run: unlock },
            { time: unlockAt + pick(60), run: () => { guard.classify({ type: "mousemove", movementX: 800 }); } },
            { time: renderAt, run: () => render.shift()?.() },
            { time: workerAt, run: () => { sendWorkerReply(); return flush; } },
        ];
        if (cancelReason) events.push({ time: pick(readyAt), run: () => cursor.cancel(cancelReason) });
        for (const { time, run } of events.sort((a, b) => a.time - b.time)) {
            t.mock.timers.tick(releaseAt + time - Date.now());
            await run();
        }
        const detail = `seed 50712, iteration ${iteration}, unlock ${unlockAt}, render ${renderAt}, worker ${workerAt}`;
        assert.equal(messages.length, 1, detail);
        assert.equal(cursor.waitingForUnlock, false, detail);
        if (cancelReason) assert.deepEqual(messages[0], { type: "cursorReturnSkipped", detail: cancelReason }, detail);
        else {
            const [message] = messages;
            assert.equal(message.type, "returnCursor", detail);
            assert.ok("destination" in message.warp, detail);
            assert.deepEqual(message.warp.destination.offsetPixels, {
                x: (Math.abs(expected.x) % 600 - origin.x) * 1.25, y: (Math.abs(expected.y) % 100 - origin.y) * 1.25,
            }, detail);
            assert.equal(message.warp.expiresAt, releaseAt + readyAt + 100, detail);
        }
    }
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
        "Sending cursor return to the extension", "Webview received native cursor return acknowledgment"]);
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

test("a canceled point drag reports its cause without requesting a release frame or cursor move", () => {
    const { cursor, messages, unlock, render } = harness();
    const detail = "Control-point drag canceled. The presentation requested cancellation.";
    cursor.finish(null, origin, undefined, "", undefined, detail);
    unlock();
    assert.deepEqual(render, []);
    assert.deepEqual(messages, [{ type: "cursorReturnSkipped", detail }]);
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

import test from "node:test";
import assert from "node:assert/strict";
import { hoverEnterEvent, hoverLeaveEvent, TrustedHover, type HoverElement, type HoverEnvironment } from "../src/trustedHover";

class FakeElement extends EventTarget implements HoverElement {
    readonly attributes = new Set<string>();
    isConnected = true;
    status = "";
    constructor(readonly name: string, readonly parentElement: FakeElement | null, log: string[]) {
        super();
        this.addEventListener(hoverEnterEvent, () => log.push(`enter ${name}`));
        this.addEventListener(hoverLeaveEvent, () => log.push(`leave ${name}`));
    }
    setAttribute(name: string) { this.attributes.add(name); }
    removeAttribute(name: string) { this.attributes.delete(name); }
}

function page(refreshed?: (target: FakeElement | null) => void) {
    const log: string[] = [];
    const editor = new FakeElement("editor", null, log);
    const lane = new FakeElement("lane", editor, log);
    const freq = new FakeElement("freq", lane, log);
    const detune = new FakeElement("detune", lane, log);
    const layout = new Map<string, FakeElement>([["10,10", freq], ["10,40", detune], ["90,90", editor]]);
    const windowTarget = new EventTarget();
    const documentTarget = new EventTarget();
    let locked = false;
    let changed = () => {};
    const frames: Array<() => void> = [];
    const env: HoverEnvironment = {
        window: windowTarget, document: documentTarget,
        locked: () => locked,
        elementAt: (point) => layout.get(`${point.x},${point.y}`) ?? null,
        viewport: () => ({ width: 100, height: 100 }),
        observeChanges: (callback) => { changed = callback; return () => {}; },
        requestFrame: (callback) => frames.push(callback),
        cancelFrame: () => {},
    };
    const hover = new TrustedHover(env, (target) => refreshed?.(target as FakeElement | null));
    const pointer = (type: string, fields: Record<string, unknown>) =>
        windowTarget.dispatchEvent(Object.assign(new Event(type), fields));
    return {
        log, layout, freq, detune, lane, editor, hover,
        move: (x: number, y: number) => pointer("pointermove", { clientX: x, clientY: y, pointerId: 1 }),
        out: (x: number, y: number) => pointer("pointerout", { clientX: x, clientY: y, relatedTarget: null }),
        capture: (got: boolean) => pointer(got ? "gotpointercapture" : "lostpointercapture", { pointerId: 1 }),
        lock: (value: boolean) => { locked = value; documentTarget.dispatchEvent(new Event("pointerlockchange")); },
        domChanged: () => changed(),
        flush: () => { while (frames.length) frames.shift()!(); },
    };
}

test("hover follows received moves, entering outermost first and leaving innermost first", () => {
    const { log, move, freq, lane, detune } = page();
    move(10, 10);
    assert.deepEqual(log, ["enter editor", "enter lane", "enter freq"]);
    assert.ok(freq.attributes.has("data-hovered") && lane.attributes.has("data-hovered"));
    move(10, 10);
    assert.equal(log.length, 3); // Moving within one element enters nothing new.
    move(10, 40);
    assert.deepEqual(log.slice(3), ["leave freq", "enter detune"]);
    assert.ok(!freq.attributes.has("data-hovered") && detune.attributes.has("data-hovered"));
});

test("after a reset, the next move enters the same knob again, so focus loss cannot strand hover", () => {
    const { log, move, hover, freq } = page();
    move(10, 10);
    hover.reset();
    assert.deepEqual(log.slice(3), ["leave freq", "leave lane", "leave editor"]);
    assert.equal(freq.attributes.size, 0);
    move(10, 10);
    assert.deepEqual(log.slice(6), ["enter editor", "enter lane", "enter freq"]);
});

test("hover freezes while locked or captured, then resumes at the latest position", () => {
    const { log, move, lock, capture, flush } = page();
    move(10, 10);
    lock(true);
    move(10, 40);
    flush();
    lock(false);
    flush();
    assert.deepEqual(log.slice(3), []); // Locked positions are not positions.
    capture(true);
    move(10, 40);
    assert.deepEqual(log.slice(3), []);
    capture(false);
    flush();
    assert.deepEqual(log.slice(3), ["leave freq", "enter detune"]);
});

test("content changing under a resting pointer, and leaving the window, update hover", () => {
    const { log, move, out, layout, domChanged, flush, freq, detune } = page();
    move(10, 10);
    layout.set("10,10", detune);
    domChanged();
    flush();
    assert.deepEqual(log.slice(3), ["leave freq", "enter detune"]);
    out(50, 50); // Still inside the viewport: not a window exit.
    assert.equal(log.length, 5);
    out(-1, 50);
    assert.deepEqual(log.slice(5), ["leave detune", "leave lane", "leave editor"]);
    assert.equal(freq.attributes.size, 0);
});

test("layout can hit-test the trusted pointer before hover transitions run", () => {
    const { hover, log, move, layout, freq, detune, capture, lock } = page();
    assert.equal(hover.elementAtPointer(), null);
    move(10, 10);
    assert.equal(hover.elementAtPointer(), freq);
    layout.set("10,10", detune);
    assert.equal(hover.elementAtPointer(), detune);
    assert.deepEqual(log, ["enter editor", "enter lane", "enter freq"]);
    capture(true);
    assert.equal(hover.elementAtPointer(), null);
    capture(false);
    lock(true);
    assert.equal(hover.elementAtPointer(), null);
});

test("status refreshes under a stationary pointer and clears when the pointer leaves", () => {
    const displayed: Array<string | null> = [];
    const { freq, detune, move, domChanged, flush, out, capture } = page((target) => displayed.push(target?.status ?? null));
    freq.status = "Frequency · 440";
    move(10, 10);
    assert.equal(displayed.at(-1), "Frequency · 440");
    freq.status = "Frequency · 441";
    detune.status = "Detune · 2";
    domChanged();
    flush();
    assert.equal(displayed.at(-1), "Frequency · 441");
    capture(true);
    move(10, 40);
    assert.equal(displayed.at(-1), "Frequency · 441");
    capture(false);
    flush();
    assert.equal(displayed.at(-1), "Detune · 2");
    out(-1, 50);
    assert.equal(displayed.at(-1), null);
});

import test from "node:test";
import assert from "node:assert/strict";
import { KnobClicks } from "../src/knobClicks";

test("two short clicks reset the same knob even when pointer lock retargets native click events", () => {
    const clicks = new KnobClicks();
    assert.equal(clicks.press("gain", { x: 10, y: 20 }, 0), false);
    clicks.move(1, 1);
    clicks.release(50);
    assert.equal(clicks.press("gain", { x: 11, y: 21 }, 150), true);
    clicks.release(200);
    assert.equal(clicks.press("gain", { x: 11, y: 21 }, 250), false);
});

test("a drag or long hold is never the first click of a reset", () => {
    for (const drag of [false, true]) {
        const clicks = new KnobClicks();
        clicks.press("gain", { x: 10, y: 20 }, 0);
        if (drag) { clicks.move(0, 10); clicks.move(0, -10); }
        clicks.release(drag ? 50 : 600);
        assert.equal(clicks.press("gain", { x: 10, y: 20 }, 700, 2), false);
    }
});

test("reset requires the same nearby knob and a new click after cancellation", () => {
    for (const [key, point, time] of [
        ["mix", { x: 10, y: 20 }, 150], ["gain", { x: 20, y: 20 }, 150],
        ["gain", { x: 10, y: 20 }, 1000],
    ] as const) {
        const clicks = new KnobClicks();
        clicks.press("gain", { x: 10, y: 20 }, 0); clicks.release(50);
        assert.equal(clicks.press(key, point, time), false);
    }
    const clicks = new KnobClicks();
    clicks.press("gain", { x: 10, y: 20 }, 0); clicks.release(50); clicks.cancel();
    assert.equal(clicks.press("gain", { x: 10, y: 20 }, 150), false);
});

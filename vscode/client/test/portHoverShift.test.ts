import test from "node:test";
import assert from "node:assert/strict";
import { initialPortHoverShift, keyboardPortHoverShift, pointerPortHoverShift } from "../src/portHoverShift";

const pointer = (type: string, shiftKey: boolean, clientX = 100, clientY = 80) =>
    ({ type, pointerId: 1, clientX, clientY, shiftKey });

test("held Shift survives row replacement without keyboard focus", () => {
    const held = pointerPortHoverShift(initialPortHoverShift(), pointer("pointermove", true));
    // Opening a port list, then its connection chooser, changes the hovered
    // element twice. Neither change represents a release of the physical key.
    for (const type of ["pointerleave", "pointerout", "pointerenter", "pointerover", "pointermove"]) {
        assert.equal(pointerPortHoverShift(held, pointer(type, false)), held);
    }
    assert.equal(held.held, true);
    const released = pointerPortHoverShift(held, pointer("pointermove", false, 101));
    assert.equal(released.held, false);
});

test("keyboard changes are not undone by stationary pointer events", () => {
    const resting = pointerPortHoverShift(initialPortHoverShift(), pointer("pointermove", false));
    const pressed = keyboardPortHoverShift(resting, true);
    assert.equal(pointerPortHoverShift(pressed, pointer("pointermove", false)), pressed);
    const released = keyboardPortHoverShift(pressed, false);
    assert.equal(pointerPortHoverShift(released, pointer("pointermove", true)), released);
    assert.equal(released.held, false);
});

test("explicit button events refresh Shift even without movement", () => {
    const held = pointerPortHoverShift(initialPortHoverShift(), pointer("pointermove", true));
    const released = pointerPortHoverShift(held, pointer("pointerdown", false));
    assert.equal(released.held, false);
    const pressed = pointerPortHoverShift(released, pointer("pointerup", true));
    assert.equal(pressed.held, true);
});

test("vertical movement and a new pointer refresh modifier state", () => {
    const held = pointerPortHoverShift(initialPortHoverShift(), pointer("pointermove", true));
    assert.equal(pointerPortHoverShift(held, pointer("pointermove", false, 100, 81)).held, false);
    assert.equal(pointerPortHoverShift(held, { ...pointer("pointermove", false), pointerId: 2 }).held, false);
});

test("after blur the first movement is observed even at the previous coordinates", () => {
    const previous = pointerPortHoverShift(initialPortHoverShift(), pointer("pointermove", true));
    const cleared = initialPortHoverShift();
    assert.equal(cleared.held, false);
    assert.equal(cleared.pointer, null);
    const resumed = pointerPortHoverShift(cleared, pointer("pointermove", true));
    assert.equal(resumed.held, previous.held);
    assert.deepEqual(resumed.pointer, previous.pointer);
});

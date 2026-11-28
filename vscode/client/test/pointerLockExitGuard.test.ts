import test from "node:test";
import assert from "node:assert/strict";
import { hoverTransition } from "../src/hoverTransition";
import { PointerLockExitGuard } from "../src/pointerLockExitGuard";

test("after a lock ends, displaced positions are dropped and actions go to the origin until real movement", () => {
    const guard = new PointerLockExitGuard();
    assert.equal(guard.classify({ type: "pointerdown" }), "pass");
    guard.begin({ x: 57, y: 175 });
    // The recorded ghost: a move with no movement to a displaced position,
    // the boundary events it causes, then a press that would hit another knob.
    assert.equal(guard.classify({ type: "pointermove", movementX: 0, movementY: 0 }), "drop");
    assert.equal(guard.classify({ type: "mousemove", movementX: 0, movementY: 0 }), "drop");
    for (const type of ["pointerout", "pointerleave", "pointerover", "pointerenter", "mouseleave", "mouseenter"])
        assert.equal(guard.classify({ type }), "drop");
    for (const type of ["pointerdown", "pointerup", "click", "dblclick", "contextmenu", "mousedown", "mouseup", "auxclick"])
        assert.equal(guard.classify({ type }), "retarget");
    assert.equal(guard.classify({ type: "wheel" }), "pass");
    assert.equal(guard.classify({ type: "keydown" }), "pass");
    assert.deepEqual(guard.origin(), { x: 57, y: 175 });
    // The first real movement re-synchronizes the host position.
    assert.equal(guard.classify({ type: "pointermove", movementX: -19, movementY: -74 }), "settle");
    assert.equal(guard.origin(), null);
    assert.equal(guard.classify({ type: "pointerdown" }), "pass");
    assert.equal(guard.classify({ type: "pointerleave" }), "pass");
});

test("one axis of movement settles, and focus loss ends the guard without settling", () => {
    const guard = new PointerLockExitGuard();
    guard.begin({ x: 1, y: 2 });
    assert.equal(guard.classify({ type: "pointermove", movementX: 0, movementY: 1 }), "settle");
    guard.begin({ x: 1, y: 2 });
    guard.end();
    assert.equal(guard.classify({ type: "pointermove", movementX: 0, movementY: 0 }), "pass");
    assert.equal(guard.origin(), null);
});

test("hover replays leave the old chain innermost first and enter the new chain outermost first", () => {
    // Chains are listed from the hit element outward.
    const from = ["freq-knob", "freq-row", "ports", "lane", "editor"];
    const to = ["detune-knob", "detune-row", "ports", "lane", "editor"];
    assert.deepEqual(hoverTransition(from, to), {
        leaving: ["freq-knob", "freq-row"],
        entering: ["detune-row", "detune-knob"],
    });
    assert.deepEqual(hoverTransition(from, from), { leaving: [], entering: [] });
    assert.deepEqual(hoverTransition([], ["a", "b"]), { leaving: [], entering: ["b", "a"] });
});

test("only mouse input is displaced: touch, pen and touch-generated mouse events pass", () => {
    const guard = new PointerLockExitGuard();
    guard.begin({ x: 1, y: 2 });
    assert.equal(guard.classify({ type: "pointerdown", pointerType: "touch" }), "pass");
    assert.equal(guard.classify({ type: "pointermove", pointerType: "pen", movementX: 0, movementY: 0 }), "pass");
    assert.equal(guard.classify({ type: "click", firesTouchEvents: true }), "pass");
    assert.equal(guard.classify({ type: "pointerdown", pointerType: "mouse" }), "retarget");
    assert.equal(guard.classify({ type: "mousedown" }), "retarget");
});

import test from "node:test";
import assert from "node:assert/strict";
import { PointerLockExitGuard } from "../src/pointerLockExitGuard";
import { PointerLockExitRedirect, relocatedPointerInit } from "../src/pointerLockExitRedirect";

type FakeEvent = MouseEvent & { stopped: boolean; prevented: boolean };
function event(type: string, fields: Record<string, unknown> = {}): FakeEvent {
    const value = {
        type, bubbles: true, cancelable: true, composed: true, detail: 1,
        clientX: 70, clientY: 220, screenX: 100, screenY: 400, movementX: 0, movementY: 0,
        ctrlKey: false, shiftKey: true, altKey: false, metaKey: false, button: 0, buttons: 1,
        stopped: false, prevented: false,
        stopImmediatePropagation() { value.stopped = true; },
        preventDefault() { value.prevented = true; },
        ...fields,
    };
    return value as unknown as FakeEvent;
}
function harness() {
    const guard = new PointerLockExitGuard();
    const delivered: Array<{ target: string; type: string; init: PointerEventInit }> = [];
    let locked = false;
    const redirect = new PointerLockExitRedirect(guard, {
        locked: () => locked,
        elementAt: (point) => ({
            dispatchEvent: (copy: Event & { init: PointerEventInit }) => {
                delivered.push({ target: `${point.x},${point.y}`, type: copy.type, init: copy.init });
                return true;
            },
        }) as unknown as EventTarget,
        copy: (original, init) => ({ type: original.type, init }) as unknown as Event,
    });
    return { guard, redirect, delivered, lock: (value: boolean) => { locked = value; } };
}

test("a mouse press at the displaced position is stopped and delivered at the lock origin", () => {
    const { guard, redirect, delivered } = harness();
    guard.begin({ x: 57, y: 175 });
    const press = event("pointerdown", { pointerId: 1, pointerType: "mouse", isPrimary: true, width: 1, height: 1, pressure: .5 });
    redirect.handle(press);
    assert.ok(press.stopped && press.prevented);
    assert.equal(delivered.length, 1);
    assert.equal(delivered[0].target, "57,175");
    assert.deepEqual(delivered[0].init, relocatedPointerInit(press, { x: 57, y: 175 }));
    assert.equal(delivered[0].init.clientX, 57);
    assert.equal(delivered[0].init.screenY, 355);
    assert.equal(delivered[0].init.pointerId, 1);
    assert.equal(delivered[0].init.shiftKey, true);
});

test("displaced moves are dropped and the first real movement passes and ends the guard", () => {
    const { guard, redirect, delivered } = harness();
    guard.begin({ x: 57, y: 175 });
    const ghost = event("pointermove", { pointerType: "mouse" });
    redirect.handle(ghost);
    assert.ok(ghost.stopped);
    const real = event("pointermove", { pointerType: "mouse", movementX: -19, movementY: -74 });
    redirect.handle(real);
    assert.ok(!real.stopped && !real.prevented);
    assert.equal(guard.origin(), null);
    assert.equal(delivered.length, 0);
});

test("a changed absolute position settles the guard even when relative deltas stay zero", () => {
    const { guard, redirect } = harness();
    guard.begin({ x: 57, y: 175 });
    const pointer = event("pointermove", { pointerType: "mouse" });
    const mouse = event("mousemove");
    const real = event("pointermove", { pointerType: "mouse", clientY: 221 });
    redirect.handle(pointer); redirect.handle(mouse); redirect.handle(real);
    assert.ok(pointer.stopped && mouse.stopped);
    assert.ok(!real.stopped);
    assert.equal(guard.origin(), null);
});

test("when the old target is gone the release can still reach the drag owner", () => {
    const guard = new PointerLockExitGuard();
    guard.begin({ x: 57, y: 175 });
    const redirect = new PointerLockExitRedirect(guard, {
        locked: () => false, elementAt: () => null, copy: () => { throw new Error("no target"); },
    });
    const release = event("pointerup", { pointerType: "mouse", button: 1 });
    const press = event("pointerdown", { pointerType: "mouse", button: 1 });
    redirect.handle(release); redirect.handle(press);
    assert.ok(!release.stopped && !release.prevented);
    assert.ok(press.stopped && press.prevented);
});

test("touch and pen input is never redirected and leaves the mouse guard active", () => {
    const { guard, redirect, delivered } = harness();
    guard.begin({ x: 57, y: 175 });
    for (const pointerType of ["touch", "pen"]) {
        const press = event("pointerdown", { pointerId: 2, pointerType });
        redirect.handle(press);
        assert.ok(!press.stopped && !press.prevented);
    }
    const compatibility = event("mousedown", { sourceCapabilities: { firesTouchEvents: true } });
    redirect.handle(compatibility);
    assert.ok(!compatibility.stopped);
    assert.equal(delivered.length, 0);
    redirect.handle(event("pointerdown", { pointerId: 1, pointerType: "mouse" }));
    assert.equal(delivered.length, 1);
});

test("locked events pass, and a re-dispatched copy is not redirected again", () => {
    const { guard, redirect, delivered, lock } = harness();
    guard.begin({ x: 57, y: 175 });
    lock(true);
    const locked = event("pointerup", { pointerType: "mouse" });
    redirect.handle(locked);
    assert.ok(!locked.stopped);
    lock(false);
    let copy: Event | null = null;
    const replaying = new PointerLockExitRedirect(guard, {
        locked: () => false,
        elementAt: () => ({ dispatchEvent: (value: Event) => { copy = value; return true; } }) as unknown as EventTarget,
        copy: (original) => event(original.type, { pointerType: "mouse" }),
    });
    replaying.handle(event("click", { pointerType: "mouse" }));
    assert.ok(copy);
    replaying.handle(copy as unknown as MouseEvent);
    assert.ok(!(copy as unknown as FakeEvent).stopped);
    assert.equal(delivered.length, 0);
});

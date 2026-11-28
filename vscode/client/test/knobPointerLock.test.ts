import test from "node:test";
import assert from "node:assert/strict";
import { KnobPointerLock } from "../src/knobPointerLock";
import type { Point } from "../src/graphScope";

function harness(delayedExit = false) {
    let locked = false;
    let exits = 0;
    let requests = 0;
    const moves: Array<[number, number]> = [];
    const ends: Array<[number, Point | null]> = [];
    const lockOrigins: Array<Point | null> = [];
    const cancellations: Array<string | undefined> = [];
    const errors: string[] = [];
    const owner = new KnobPointerLock({
        request: () => { ++requests; }, exit: () => { ++exits; if (!delayedExit) locked = false; }, locked: () => locked,
    }, (id, delta) => moves.push([id, delta]), (id, point, origin, cancellation) => {
        ends.push([id, point]); lockOrigins.push(origin); cancellations.push(cancellation);
    }, (error) => errors.push(error));
    return { owner, moves, ends, lockOrigins, cancellations, errors, exits: () => exits, requests: () => requests,
        lock: (value: boolean, announce = true) => { locked = value; if (announce) owner.changed(); } };
}

test("locked knob movement is relative and release restores its original hover position", () => {
    const { owner, moves, ends, lock, exits } = harness();
    owner.start(7, { x: 20, y: 30 });
    assert.equal(owner.suppressPointerCancellation(), true);
    lock(true);
    owner.move(10000);
    owner.move(-20000);
    assert.deepEqual(moves, [[7, 10000], [7, -20000]]);
    owner.release(8, { x: 1, y: 1 });
    assert.deepEqual(ends, []);
    owner.release(7, { x: 9999, y: 9999 });
    assert.deepEqual(ends, [[7, { x: 20, y: 30 }]]);
    assert.equal(exits(), 1);
    lock(false);
    assert.equal(ends.length, 1);
});

test("a lock granted after mouse release is exited without restarting the gesture", () => {
    const { owner, ends, lock, exits } = harness();
    owner.start(1, { x: 20, y: 30 });
    owner.release(1, { x: 21, y: 31 });
    lock(true);
    assert.equal(exits(), 1);
    assert.deepEqual(ends, [[1, { x: 21, y: 31 }]]);
    assert.equal(owner.move(100), false);
});

test("release after the browser grants lock but before its change event restores the cursor and allows another drag", () => {
    const { owner, ends, lockOrigins, lock, exits, requests, moves } = harness(true);
    const origin = { x: 20, y: 30 };
    owner.start(1, origin);
    lock(true, false);
    owner.release(1, { x: 40, y: 50 });
    assert.deepEqual(ends, [[1, origin]]);
    assert.deepEqual(lockOrigins, [origin]);
    assert.equal(exits(), 1);

    owner.start(2, { x: 25, y: 35 });
    lock(false);
    assert.equal(requests(), 2);
    lock(true);
    assert.equal(owner.move(16), true);
    assert.deepEqual(moves, [[2, 16]]);
});

test("focus loss ends the drag without retaining hover and always releases the pointer", () => {
    const { owner, ends, lock, exits } = harness();
    owner.start(1, { x: 20, y: 30 }); lock(true);
    owner.cancel(); owner.cancel();
    assert.deepEqual(ends, [[1, null]]);
    assert.equal(exits(), 1);
});

test("locked point movement that reports the drag button up finishes once at the lock origin", () => {
    const { owner, ends, moves, lock, lockOrigins, cancellations, exits } = harness();
    const origin = { x: 20, y: 30 };
    owner.start(7, origin); lock(true);
    assert.equal(owner.move(5, { buttons: 1, mask: 1 }), true);
    assert.equal(owner.move(8, { buttons: 2, mask: 1 }), true);
    owner.release(7, origin);
    assert.deepEqual(moves, [[7, 5]]);
    assert.deepEqual(ends, [[7, origin]]);
    assert.deepEqual(lockOrigins, [origin]);
    assert.deepEqual(cancellations, [undefined]);
    assert.equal(exits(), 1);
});

test("button release still finishes normally when the browser clears its lock before dispatching the change", () => {
    const { owner, ends, lock, lockOrigins, cancellations } = harness();
    const origin = { x: 20, y: 30 };
    owner.start(7, origin); lock(true);
    lock(false, false);
    owner.release(7, { x: 40, y: 50 });
    owner.changed();
    assert.deepEqual(ends, [[7, origin]]);
    assert.deepEqual(lockOrigins, [origin]);
    assert.deepEqual(cancellations, [undefined]);
});

test("a failing finish callback still exits pointer lock", () => {
    let locked = false;
    const owner = new KnobPointerLock({
        request: () => {}, exit: () => { locked = false; }, locked: () => locked,
    }, () => {}, () => { throw new Error("save failed"); }, () => {});
    owner.start(1, { x: 20, y: 30 }); locked = true; owner.changed();
    assert.throws(() => owner.release(1, { x: 22, y: 32 }), /save failed/);
    assert.equal(locked, false);
});

test("an external unlock finishes once, and a denied request keeps bounded dragging available", () => {
    const { owner, ends, cancellations, errors, lock } = harness();
    owner.start(1, { x: 20, y: 30 }); lock(true); lock(false);
    assert.deepEqual(ends, [[1, null]]);
    assert.deepEqual(cancellations, ["The browser released pointer lock before the editor received a button release."]);
    owner.start(2, { x: 10, y: 10 }); owner.error("denied");
    assert.deepEqual(errors, ["denied"]);
    assert.equal(owner.suppressPointerCancellation(), false);
    owner.release(2, { x: 15, y: 15 });
    assert.deepEqual(ends[1], [2, { x: 15, y: 15 }]);
});

test("a quick regrab discards the previous press's pending lock and obtains its own anchor", () => {
    const { owner, requests, lock, moves, exits } = harness();
    owner.start(1, { x: 10, y: 10 });
    owner.release(1, { x: 10, y: 10 });
    owner.start(2, { x: 20, y: 20 });
    assert.equal(requests(), 1);
    lock(true);
    assert.equal(exits(), 1);
    assert.equal(owner.move(99), false);
    lock(false);
    assert.equal(requests(), 2);
    lock(true);
    owner.move(100);
    assert.deepEqual(moves, [[2, 100]]);
});

test("a second press cannot steal a held drag, even with a reused mouse pointer id", () => {
    const { owner, lock, moves, ends } = harness();
    owner.start(1, { x: 10, y: 20 }); lock(true);
    owner.start(1, { x: 10, y: 50 });
    owner.move(5);
    owner.release(1, { x: 10, y: 50 });
    assert.deepEqual(moves, [[1, 5]]);
    assert.deepEqual(ends, [[1, { x: 10, y: 20 }]]);
});

test("the old drag's delayed unlock cannot terminate the next drag", () => {
    const { owner, requests, lock, moves, ends } = harness(true);
    owner.start(1, { x: 10, y: 10 }); lock(true);
    owner.release(1, { x: 10, y: 10 });
    owner.start(2, { x: 20, y: 20 });
    assert.equal(owner.move(99), false);
    assert.equal(requests(), 1);
    lock(false);
    assert.equal(requests(), 2);
    assert.deepEqual(ends, [[1, { x: 10, y: 10 }]]);
    lock(true); owner.move(100);
    assert.deepEqual(moves, [[2, 100]]);
});

test("failure of an obsolete request does not deny the new grab its own acquisition", () => {
    const { owner, requests, lock, errors, moves } = harness();
    owner.start(1, { x: 10, y: 10 }); owner.release(1, { x: 10, y: 10 });
    owner.start(2, { x: 20, y: 20 });
    owner.error("old request was interrupted");
    assert.equal(requests(), 2);
    assert.deepEqual(errors, []);
    lock(true); owner.move(100);
    assert.deepEqual(moves, [[2, 100]]);
});

test("a denied first press can retry after native focus arrives, but an external escape never relocks", () => {
    const { owner, requests, lock, ends } = harness();
    owner.start(1, { x: 10, y: 10 }); owner.error("not focused yet");
    owner.retryAfterFocus(); owner.retryAfterFocus();
    assert.equal(requests(), 2);
    lock(true); lock(false);
    owner.retryAfterFocus();
    assert.equal(requests(), 2);
    assert.deepEqual(ends, [[1, null]]);
});

test("relative travel remains unbounded while locked", () => {
    const { owner, lock, errors, moves } = harness();
    owner.start(1, { x: 100, y: 100 }); lock(true);
    owner.move(10000);
    owner.move(10000);
    assert.deepEqual(errors, []);
    assert.deepEqual(moves, [[1, 10000], [1, 10000]]);
});

test("a browser rejection preserves its exception name and leaves the drag available for focus recovery", async () => {
    let requests = 0;
    let locked = false;
    const errors: string[] = [];
    const moves: number[] = [];
    const ends: Array<Point | null> = [];
    const owner = new KnobPointerLock({
        request: () => {
            if (++requests === 1) return Promise.reject(new DOMException(
                "The root document of this element is not valid for pointer lock.", "WrongDocumentError"));
        },
        exit: () => { locked = false; }, locked: () => locked,
    }, (_id, delta) => moves.push(delta), (_id, point) => ends.push(point), (message) => errors.push(message));
    owner.start(1, { x: 10, y: 20 });
    await Promise.resolve();
    assert.deepEqual(errors, ["WrongDocumentError: The root document of this element is not valid for pointer lock."]);
    assert.deepEqual(ends, []);
    owner.retryAfterFocus();
    assert.equal(requests, 2);
    locked = true; owner.changed(); owner.move(100);
    assert.deepEqual(moves, [100]);
    owner.release(1, { x: 999, y: 999 });
    assert.deepEqual(ends, [{ x: 10, y: 20 }]);
});

test("only a drag whose pointer lock was granted reports the lock origin the cursor returns to", () => {
    const scenario = (run: (owner: KnobPointerLock, lock: (value: boolean) => void) => void) => {
        const origins: Array<Point | null> = [];
        let locked = false;
        const owner = new KnobPointerLock({ request: () => {}, exit: () => { locked = false; }, locked: () => locked },
            () => {}, (_id, _point, lockOrigin) => origins.push(lockOrigin), () => {});
        run(owner, (value) => { locked = value; owner.changed(); });
        return origins;
    };
    // Released before the grant: the cursor never moved.
    assert.deepEqual(scenario((owner) => { owner.start(1, { x: 20, y: 30 }); owner.release(1, { x: 21, y: 31 }); }), [null]);
    assert.deepEqual(scenario((owner, lock) => {
        owner.start(2, { x: 40, y: 50 }); lock(true); owner.release(2, { x: 99, y: 99 });
    }), [{ x: 40, y: 50 }]);
    // Escape or the host ended the lock before release.
    assert.deepEqual(scenario((owner, lock) => {
        owner.start(3, { x: 60, y: 70 }); lock(true); lock(false);
    }), [{ x: 60, y: 70 }]);
    assert.deepEqual(scenario((owner, lock) => {
        owner.start(4, { x: 80, y: 90 }); lock(true); owner.cancel();
    }), [{ x: 80, y: 90 }]);
});

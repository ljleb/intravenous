import test from "node:test";
import assert from "node:assert/strict";
import { initialTimelineViewport, maximumLaneHeight, minimumLaneHeight, moveMiddlePan,
    readTimelineViewport, timelineWheelAction, wheelPixels, zoomTimelineHorizontally,
    zoomTimelineVertically } from "../src/timelineViewport";

test("timeline wheel modifiers implement the requested bindings", () => {
    const plain = { ctrlKey: false, altKey: false, shiftKey: false };
    assert.equal(timelineWheelAction(plain), "verticalScroll");
    assert.equal(timelineWheelAction({ ...plain, altKey: true }), "verticalZoom");
    assert.equal(timelineWheelAction({ ...plain, shiftKey: true }), "horizontalScroll");
    assert.equal(timelineWheelAction({ ...plain, ctrlKey: true }), "horizontalZoom");
    assert.equal(wheelPixels(2, 0, 400), 2);
    assert.equal(wheelPixels(2, 1, 400), 32);
    assert.equal(wheelPixels(2, 2, 400), 800);
});

test("horizontal zoom preserves the time under the pointer and vertical geometry", () => {
    const view = { ...initialTimelineViewport, beginSeconds: -4, scrollTop: 500 };
    for (const delta of [-100000, -80, 80, 100000]) {
        const next = zoomTimelineHorizontally(view, 300, delta);
        assert.ok(Math.abs(next.beginSeconds + 300 * next.secondsPerPixel
            - (view.beginSeconds + 300 * view.secondsPerPixel)) < 1e-6);
        assert.equal(next.laneHeight, view.laneHeight);
        assert.equal(next.scrollTop, view.scrollTop);
    }
});

test("vertical zoom preserves the lane position under the pointer within scroll bounds", () => {
    const view = { ...initialTimelineViewport, scrollTop: 600 };
    for (const delta of [-100, 100]) {
        const next = zoomTimelineVertically(view, 100, delta);
        assert.ok(Math.abs((next.scrollTop + 100) / next.laneHeight
            - (view.scrollTop + 100) / view.laneHeight) < 1e-9);
        assert.equal(next.beginSeconds, view.beginSeconds);
        assert.equal(next.secondsPerPixel, view.secondsPerPixel);
    }
    assert.equal(zoomTimelineVertically(view, 0, 1e9).laneHeight, minimumLaneHeight);
    assert.equal(zoomTimelineVertically(view, 0, -1e9).laneHeight, maximumLaneHeight);
    assert.equal(zoomTimelineVertically(initialTimelineViewport, 1000, 1000).scrollTop, 0);
});

test("middle movement pans both axes only while the middle button is held", () => {
    const view = { ...initialTimelineViewport, beginSeconds: 2, scrollTop: 500 };
    const pan = { pointerId: 1, x: 100, y: 100, view };
    assert.deepEqual(moveMiddlePan(pan, 4, 100, 100), view);
    const moved = moveMiddlePan(pan, 4, 150, 175)!;
    assert.equal(moved.beginSeconds, 1.5);
    assert.equal(moved.scrollTop, 425);
    assert.equal(moved.laneHeight, view.laneHeight);
    assert.equal(moveMiddlePan(pan, 0, 150, 175), null);
    assert.equal(moveMiddlePan(pan, 1, 150, 175), null);
});

test("saved timeline state normalizes invalid values and does not depend on viewport size", () => {
    const saved = { beginSeconds: -3, secondsPerPixel: 0.02, laneHeight: 360, scrollTop: 50 };
    assert.deepEqual(readTimelineViewport(saved), saved);
    assert.deepEqual(readTimelineViewport(undefined), initialTimelineViewport);
    assert.deepEqual(readTimelineViewport({ beginSeconds: NaN, secondsPerPixel: -1, laneHeight: Infinity, scrollTop: -4 }),
        initialTimelineViewport);
});

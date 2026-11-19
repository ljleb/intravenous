import test from "node:test";
import assert from "node:assert/strict";
import { initialTimelineViewport, isMiddleDoubleClick, maximumLaneHeight, middleClickMode, middleDragZoomFactor, middleZoomPickup,
    minimumLaneHeight, moveMiddleDrag, panTimeline, readTimelineViewport, resetTimelineZoom, timelineMiddleDragAction, timelineWheelAction,
    wheelPixels, wrapTimelineCursor, zoomAnchorHint, zoomMiddleDrag, zoomMiddleDragAtPickup, zoomTimelineAxes,
    zoomTimelineHorizontally, zoomTimelineVertically, type MiddleDrag } from "../src/timelineViewport";

test("timeline wheel modifiers implement the requested bindings", () => {
    const plain = { ctrlKey: false, altKey: false, shiftKey: false };
    assert.equal(timelineWheelAction(plain), "verticalScroll");
    assert.equal(timelineWheelAction({ ...plain, altKey: true }), "verticalZoom");
    assert.equal(timelineWheelAction({ ...plain, shiftKey: true }), "horizontalScroll");
    assert.equal(timelineWheelAction({ ...plain, ctrlKey: true }), "horizontalZoom");
    const altShift = { ...plain, altKey: true, shiftKey: true };
    assert.equal(timelineWheelAction(altShift), "verticalZoom");
    assert.equal(timelineWheelAction(altShift, true), "laneValueZoom");
    assert.equal(timelineWheelAction({ ...altShift, ctrlKey: true }, true), "bothZoom");
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
    const pan: MiddleDrag = { pointerId: 1, x: 100, y: 100, action: "pan" };
    assert.equal(moveMiddleDrag(view, pan, 4, 100, 100, { x: 0, y: 0 }), view);
    const moved = moveMiddleDrag(view, pan, 4, 150, 175, { x: 0, y: 0 })!;
    assert.equal(moved.beginSeconds, 1.5);
    assert.equal(moved.scrollTop, 425);
    assert.equal(moved.laneHeight, view.laneHeight);
    assert.equal(moveMiddleDrag(view, pan, 0, 150, 175, { x: 0, y: 0 }), null);
    assert.equal(moveMiddleDrag(view, pan, 1, 150, 175, { x: 0, y: 0 }), null);
});

test("a lane value pan moves graph time horizontally and leaves graph scroll fixed", () => {
    const view = { ...initialTimelineViewport, beginSeconds: 2, scrollTop: 500 };
    const pan: MiddleDrag = { pointerId: 1, x: 100, y: 100, action: "pan" };
    assert.deepEqual(moveMiddleDrag(view, pan, 4, 150, 175, { x: 0, y: 0 }, true), { ...view, beginSeconds: 1.5 });
    assert.equal(moveMiddleDrag(view, pan, 4, 100, 175, { x: 0, y: 0 }, true), view);
    assert.equal(moveMiddleDrag(view, pan, 0, 150, 175, { x: 0, y: 0 }, true), null);
});

test("panning follows mouse travel across a viewport or lane wrap", () => {
    const view = { ...initialTimelineViewport, beginSeconds: 2, scrollTop: 500 };
    const delta = { x: 5, y: 4 };
    const viewport = { left: 20, top: 0, width: 200, height: 300 };
    const lane = { ...viewport, top: 36, height: 48 };
    assert.deepEqual(wrapTimelineCursor({ x: 217, y: 298 }, delta, viewport).visible, { x: 22, y: 2 });
    assert.deepEqual(wrapTimelineCursor({ x: 217, y: 82 }, delta, lane).visible, { x: 22, y: 38 });
    assert.deepEqual(panTimeline(view, delta, false), { ...view, beginSeconds: 1.95, scrollTop: 496 });
    assert.deepEqual(panTimeline(view, delta, true), { ...view, beginSeconds: 1.95 });
});

test("middle drag selects the zoom axis with the same modifier precedence as the wheel", () => {
    assert.equal(timelineMiddleDragAction({ ctrlKey: false, altKey: false }), "pan");
    assert.equal(timelineMiddleDragAction({ ctrlKey: false, altKey: true }), "verticalZoom");
    assert.equal(timelineMiddleDragAction({ ctrlKey: true, altKey: false }), "horizontalZoom");
    assert.equal(timelineMiddleDragAction({ ctrlKey: true, altKey: true }), "bothZoom");
});

test("combined middle zoom scales each axis separately at the initial pickup", () => {
    const view = { ...initialTimelineViewport, beginSeconds: -4, scrollTop: 500 };
    const point = { x: 300, y: 120 };
    const pickup = middleZoomPickup(view, point, { x: 450, y: 180 });
    const axes = { horizontal: true, vertical: true };
    const horizontal = zoomMiddleDragAtPickup(view, { x: 40, y: 0 }, pickup, axes);
    assert.ok(horizontal.secondsPerPixel > view.secondsPerPixel);
    assert.equal(horizontal.laneHeight, view.laneHeight);
    assert.equal(horizontal.scrollTop, view.scrollTop);
    const both = zoomMiddleDragAtPickup(horizontal, { x: 0, y: 20 }, pickup, axes);
    assert.equal(both.beginSeconds, horizontal.beginSeconds);
    assert.equal(both.secondsPerPixel, horizontal.secondsPerPixel);
    assert.ok(both.secondsPerPixel > view.secondsPerPixel);
    assert.ok(both.laneHeight < view.laneHeight);
    assert.ok(Math.abs(both.beginSeconds + point.x * both.secondsPerPixel - pickup.timeSeconds) < 1e-9);
    assert.ok(Math.abs((both.scrollTop + point.y) / both.laneHeight - pickup.lanePosition) < 1e-9);
    assert.deepEqual(zoomMiddleDragAtPickup(view, { x: 40, y: 20 }, pickup, axes), both);
    assert.deepEqual(zoomMiddleDragAtPickup(view, { x: 40, y: 20 }, pickup, { horizontal: true, vertical: false }), horizontal);
    assert.deepEqual(zoomMiddleDragAtPickup(view, { x: 40, y: 20 }, pickup, { horizontal: false, vertical: true }),
        zoomMiddleDragAtPickup(view, { x: 0, y: 20 }, pickup, axes));
});

test("combined middle zoom keeps the original lane anchor when reversing from the scroll boundary", () => {
    const view = { ...initialTimelineViewport, scrollTop: 20 };
    const point = { x: 100, y: 100 };
    const pickup = middleZoomPickup(view, point, point);
    const axes = { horizontal: true, vertical: true };
    const clamped = zoomMiddleDragAtPickup(view, { x: 0, y: 100 }, pickup, axes);
    assert.equal(clamped.scrollTop, 0);
    const reversed = zoomMiddleDragAtPickup(clamped, { x: 0, y: -100 }, pickup, axes);
    assert.ok(Math.abs(reversed.laneHeight - view.laneHeight) < 1e-9);
    assert.ok(Math.abs(reversed.scrollTop - view.scrollTop) < 1e-9);
});

test("wheel zoom can scale both project axes with one delta", () => {
    const view = { ...initialTimelineViewport, beginSeconds: -4, scrollTop: 500 };
    const anchor = { x: 300, y: 120 };
    assert.deepEqual(zoomTimelineAxes(view, anchor, 180, { horizontal: true, vertical: true }),
        zoomTimelineVertically(zoomTimelineHorizontally(view, anchor.x, 180), anchor.y, 180));
    assert.deepEqual(zoomTimelineAxes(view, anchor, 180, { horizontal: true, vertical: false }), zoomTimelineHorizontally(view, anchor.x, 180));
    assert.deepEqual(zoomTimelineAxes(view, anchor, 180, { horizontal: false, vertical: true }), zoomTimelineVertically(view, anchor.y, 180));
    assert.equal(zoomTimelineAxes(view, anchor, 180, { horizontal: false, vertical: false }), view);
});

test("horizontal middle zoom uses vertical motion and the supplied horizontal anchor", () => {
    const view = { ...initialTimelineViewport, beginSeconds: 2, scrollTop: 500 };
    const drag: MiddleDrag = { pointerId: 1, x: 100, y: 100, action: "horizontalZoom" };
    assert.equal(moveMiddleDrag(view, drag, 4, 180, 100, { x: 80, y: 0 }), view);
    assert.equal(moveMiddleDrag(view, drag, 0, 180, 150, { x: 80, y: 0 }), null);
    for (const dy of [-50, 50]) {
        assert.deepEqual(moveMiddleDrag(view, drag, 4, 180, 100 + dy, { x: 80, y: 0 }), zoomTimelineHorizontally(view, 80, dy * 3));
    }
    const first = moveMiddleDrag(view, drag, 4, 120, 150, { x: 80, y: 0 })!;
    const second = moveMiddleDrag(first, { ...drag, x: 120, y: 150 }, 4, 140, 175, { x: 140, y: 0 });
    assert.deepEqual(second, zoomTimelineHorizontally(first, 140, 75));
});

test("vertical middle zoom uses horizontal motion and the supplied vertical anchor", () => {
    const view = { ...initialTimelineViewport, beginSeconds: 2, scrollTop: 500 };
    const drag: MiddleDrag = { pointerId: 1, x: 100, y: 100, action: "verticalZoom" };
    assert.equal(moveMiddleDrag(view, drag, 4, 100, 180, { y: 80, x: 0 }), view);
    assert.equal(moveMiddleDrag(view, drag, 0, 150, 180, { y: 80, x: 0 }), null);
    for (const dx of [-50, 50]) {
        assert.deepEqual(moveMiddleDrag(view, drag, 4, 100 + dx, 180, { y: 80, x: 0 }), zoomTimelineVertically(view, 80, dx * 3));
    }
    const first = moveMiddleDrag(view, drag, 4, 150, 120, { y: 80, x: 0 })!;
    const second = moveMiddleDrag(first, { ...drag, x: 150, y: 120 }, 4, 175, 140, { y: 140, x: 0 });
    assert.deepEqual(second, zoomTimelineVertically(first, 140, 75));
});

test("lane editors use the same middle drag zoom factor as the graph", () => {
    const view = initialTimelineViewport;
    const left = middleDragZoomFactor(-50), right = middleDragZoomFactor(50);
    assert.ok(left > 1);
    assert.ok(right < 1);
    assert.equal(middleDragZoomFactor(0), 1);
    assert.equal(zoomMiddleDrag(view, "verticalZoom", { x: -50, y: 25 }, 80).laneHeight, view.laneHeight * left);
    assert.equal(zoomMiddleDrag(view, "verticalZoom", { x: 50, y: 25 }, 80).laneHeight, view.laneHeight * right);
});

test("vertical middle zoom can reverse immediately after hitting the scale limit", () => {
    const view = { ...initialTimelineViewport, laneHeight: minimumLaneHeight };
    const drag: MiddleDrag = { pointerId: 1, x: 300, y: 100, action: "verticalZoom" };
    const clamped = moveMiddleDrag(view, drag, 4, 500, 100, { y: 80, x: 0 })!;
    assert.equal(clamped.laneHeight, minimumLaneHeight);
    const reversed = moveMiddleDrag(clamped, { ...drag, x: 500 }, 4, 499, 100, { y: 80, x: 0 })!;
    assert.ok(reversed.laneHeight > minimumLaneHeight);
});

test("the locked zoom cursor wraps in both directions while zoom follows the original motion", () => {
    const bounds = { left: 100, top: 60, width: 200, height: 100 };
    const view = { ...initialTimelineViewport, scrollTop: 500 };
    const cursor = { x: 295, y: 63 };
    const movement = { x: 10, y: -5 };
    const wrapped = wrapTimelineCursor(cursor, movement, bounds);
    assert.deepEqual(wrapped.visible, { x: 105, y: 158 });
    assert.deepEqual(wrapped.virtual, { x: 305, y: 58 });
    const repeated = wrapTimelineCursor(cursor, { x: 410, y: -305 }, bounds);
    assert.deepEqual(repeated.visible, wrapped.visible);
    assert.deepEqual(repeated.virtual, { x: 705, y: -242 });
    assert.deepEqual(repeated.screens, { x: 3, y: -4 });
    assert.equal(zoomAnchorHint("horizontalZoom", repeated.screens), "Time anchor · 3 screens right");
    assert.equal(zoomAnchorHint("verticalZoom", repeated.screens), "Lane anchor · 4 screens above");
    assert.equal(zoomAnchorHint("valueZoom", repeated.screens), "Value anchor · 4 screens above");
    const opposite = wrapTimelineCursor(cursor, { x: -410, y: 305 }, bounds);
    assert.deepEqual(opposite.virtual, { x: -115, y: 368 });
    assert.equal(zoomAnchorHint("horizontalZoom", opposite.screens), "Time anchor · 2 screens left");
    assert.equal(zoomAnchorHint("verticalZoom", opposite.screens), "Lane anchor · 3 screens below");
    assert.equal(zoomAnchorHint("verticalZoom", wrapped.screens), "Lane anchor · 1 screen above");
    assert.equal(zoomAnchorHint("horizontalZoom", { x: 0, y: 2 }), "Time anchor · in view");
    assert.equal(zoomAnchorHint("valueZoom", { x: 4, y: 0 }), "Value anchor · in view");

    const anchorX = repeated.virtual.x - bounds.left, anchorY = repeated.virtual.y - bounds.top;
    assert.deepEqual(zoomMiddleDrag(view, "horizontalZoom", movement, anchorX), zoomTimelineHorizontally(view, anchorX, -15));
    assert.deepEqual(zoomMiddleDrag(view, "verticalZoom", movement, anchorY), zoomTimelineVertically(view, anchorY, 30));
    assert.equal(zoomMiddleDrag(view, "horizontalZoom", { x: 10, y: 0 }, anchorX), view);
    assert.equal(zoomMiddleDrag(view, "verticalZoom", { x: 0, y: -5 }, anchorY), view);
});

test("middle double-click modifiers reset the selected axis at the click anchor", () => {
    const plain = { ctrlKey: false, altKey: false, shiftKey: false };
    assert.equal(middleClickMode(plain), "view");
    assert.equal(middleClickMode({ ...plain, shiftKey: true }), "lane");
    assert.equal(middleClickMode({ ...plain, ctrlKey: true }), "verticalZoom");
    assert.equal(middleClickMode({ ...plain, altKey: true }), "horizontalZoom");
    assert.equal(middleClickMode({ ...plain, altKey: true, shiftKey: true }, true), "lane");
    assert.equal(middleClickMode({ ...plain, altKey: true, shiftKey: true }), "horizontalZoom");
    const first = { x: 100, y: 120, at: 10, mode: "verticalZoom" as const, lane: null };
    assert.equal(isMiddleDoubleClick(first, { ...first, at: 300, x: 103 }), true);
    assert.equal(isMiddleDoubleClick(first, { ...first, at: 500 }), false);
    assert.equal(isMiddleDoubleClick(first, { ...first, x: 120 }), false);
    assert.equal(isMiddleDoubleClick(first, { ...first, mode: "horizontalZoom" }), false);
    const view = { beginSeconds: 2, secondsPerPixel: .02, laneHeight: 120, scrollTop: 240 };
    assert.deepEqual(resetTimelineZoom(view, "horizontalZoom", 100), { ...view, beginSeconds: 3, secondsPerPixel: .01 });
    assert.deepEqual(resetTimelineZoom(view, "verticalZoom", 120), { ...view, laneHeight: 240, scrollTop: 600 });
});

test("saved timeline state normalizes invalid values and does not depend on viewport size", () => {
    const saved = { beginSeconds: -3, secondsPerPixel: 0.02, laneHeight: 360, scrollTop: 50 };
    assert.deepEqual(readTimelineViewport(saved), saved);
    assert.deepEqual(readTimelineViewport(undefined), initialTimelineViewport);
    assert.deepEqual(readTimelineViewport({ beginSeconds: NaN, secondsPerPixel: -1, laneHeight: Infinity, scrollTop: -4 }),
        initialTimelineViewport);
});

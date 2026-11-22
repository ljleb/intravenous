import type { Point } from "./graphScope";

export type TimelineViewport = {
    beginSeconds: number;
    secondsPerPixel: number;
    laneHeight: number;
    scrollTop: number;
};
export const minimumLaneHeight = 12;
export const maximumLaneHeight = 1600;
export const editableHeightPx = 48;
export const timelineLaneBorderPx = 1;
export const verticalWheelStepPixels = 100;
const wheelZoomSensitivity = .001;
const verticalWheelLogStep = verticalWheelStepPixels * wheelZoomSensitivity;
const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));
const laneHeightAtWheelStep = (steps: number) => clamp(maximumLaneHeight * Math.exp(-steps * verticalWheelLogStep),
    minimumLaneHeight, maximumLaneHeight);
const laneWheelStepAtHeight = (height: number) => Math.log(maximumLaneHeight / height) / verticalWheelLogStep;
export const initialTimelineViewport: TimelineViewport = {
    beginSeconds: 0, secondsPerPixel: 0.01, scrollTop: 0,
    laneHeight: laneHeightAtWheelStep(Math.floor(laneWheelStepAtHeight(editableHeightPx + timelineLaneBorderPx))),
};

export function formatPlaybackSeconds(seconds: number, fits: (text: string) => boolean): string {
    const full = seconds.toFixed(3);
    if (fits(full)) return full;
    const fraction = full.lastIndexOf(".");
    if (fraction < 0) return full;
    for (let places = 2; places > 0; --places) {
        const shortened = full.slice(0, fraction + 1 + places);
        if (fits(shortened)) return shortened;
    }
    const whole = full.slice(0, fraction);
    return fits(whole) ? whole : seconds.toExponential(0).replace("e+", "e");
}

export function readTimelineViewport(value: Partial<TimelineViewport> | undefined): TimelineViewport {
    const finiteOr = (candidate: unknown, fallback: number) =>
        typeof candidate === "number" && Number.isFinite(candidate) ? candidate : fallback;
    const secondsPerPixel = finiteOr(value?.secondsPerPixel, initialTimelineViewport.secondsPerPixel);
    return {
        beginSeconds: finiteOr(value?.beginSeconds, initialTimelineViewport.beginSeconds),
        secondsPerPixel: secondsPerPixel > 0 ? clamp(secondsPerPixel, 1e-6, 86400) : initialTimelineViewport.secondsPerPixel,
        laneHeight: clamp(finiteOr(value?.laneHeight, initialTimelineViewport.laneHeight), minimumLaneHeight, maximumLaneHeight),
        scrollTop: Math.max(0, finiteOr(value?.scrollTop, 0)),
    };
}

export type TimelineZoomAction = "verticalZoom" | "horizontalZoom";
type CombinedTimelineZoom = TimelineZoomAction | "bothZoom";
export type TimelineWheelAction = "verticalScroll" | "horizontalScroll" | "laneValueZoom" | CombinedTimelineZoom;
type ZoomModifiers = { ctrlKey: boolean; altKey: boolean };
function timelineZoomAction(modifiers: ZoomModifiers): CombinedTimelineZoom | null {
    if (modifiers.ctrlKey && modifiers.altKey) return "bothZoom";
    return modifiers.ctrlKey ? "horizontalZoom" : modifiers.altKey ? "verticalZoom" : null;
}
export function timelineWheelAction(modifiers: ZoomModifiers & { shiftKey: boolean }, laneValueNavigation = false): TimelineWheelAction {
    const zoom = timelineZoomAction(modifiers);
    if (zoom === "verticalZoom" && modifiers.shiftKey && laneValueNavigation) return "laneValueZoom";
    return zoom ?? (modifiers.shiftKey ? "horizontalScroll" : "verticalScroll");
}

export function wheelPixels(delta: number, mode: number, pageSize: number): number {
    return delta * (mode === 1 ? 16 : mode === 2 ? pageSize : 1);
}

export function zoomFactorForPixels(pixels: number): number {
    return Math.exp(clamp(pixels * wheelZoomSensitivity, -20, 20));
}

export function zoomTimelineHorizontally(view: TimelineViewport, pointerX: number, delta: number): TimelineViewport {
    const secondsPerPixel = clamp(view.secondsPerPixel * zoomFactorForPixels(delta), 1e-6, 86400);
    return { ...view, secondsPerPixel,
        beginSeconds: view.beginSeconds + pointerX * (view.secondsPerPixel - secondsPerPixel) };
}

export function zoomTimelineVertically(view: TimelineViewport, pointerY: number, delta: number): TimelineViewport {
    return scaleTimelineLanes(view, pointerY, zoomFactorForPixels(-delta));
}

export function stepTimelineVerticalZoom(view: TimelineViewport, pointerY: number, steps: number): TimelineViewport {
    if (steps === 0) return view;
    const from = laneWheelStepAtHeight(view.laneHeight);
    const step = steps > 0 ? Math.floor(from + 1e-9) : Math.ceil(from - 1e-9);
    return scaleTimelineLanes(view, pointerY, laneHeightAtWheelStep(step + steps) / view.laneHeight);
}

export function zoomTimelineAxes(view: TimelineViewport, anchor: Point, delta: number,
    axes: { horizontal: boolean; vertical: boolean }): TimelineViewport {
    if (delta === 0) return view;
    const horizontal = axes.horizontal ? zoomTimelineHorizontally(view, anchor.x, delta) : view;
    return axes.vertical ? zoomTimelineVertically(horizontal, anchor.y, delta) : horizontal;
}

function scaleTimelineLanes(view: TimelineViewport, pointerY: number, factor: number): TimelineViewport {
    const laneHeight = clamp(view.laneHeight * factor, minimumLaneHeight, maximumLaneHeight);
    const lanePosition = (view.scrollTop + pointerY) / view.laneHeight;
    return { ...view, laneHeight, scrollTop: Math.max(0, lanePosition * laneHeight - pointerY) };
}

export type MiddleZoomPickup = { point: Point; cursor: Point; timeSeconds: number; lanePosition: number };
type MiddleDragPointer = Point & { pointerId: number };
type SingleMiddleDrag = MiddleDragPointer & { action: "pan" | TimelineZoomAction };
export type MiddleDrag = SingleMiddleDrag | (MiddleDragPointer & { action: "bothZoom"; zoomPickup: MiddleZoomPickup });
export type MiddleClick = Point & { at: number; mode: TimelineZoomAction | "lane" | "view"; lane: Element | null };
export function middleClickMode(modifiers: ZoomModifiers & { shiftKey: boolean }, laneValueNavigation = false): MiddleClick["mode"] {
    if (modifiers.ctrlKey) return "verticalZoom";
    if (modifiers.shiftKey && (!modifiers.altKey || laneValueNavigation)) return "lane";
    if (modifiers.altKey) return "horizontalZoom";
    return "view";
}
export function isMiddleDoubleClick(first: MiddleClick | null, second: MiddleClick): boolean {
    return first !== null && first.mode === second.mode && first.lane === second.lane
        && second.at - first.at <= 400 && Math.hypot(first.x - second.x, first.y - second.y) <= 6;
}
export function resetTimelineZoom(view: TimelineViewport, axis: TimelineZoomAction, anchor: number): TimelineViewport {
    if (axis === "horizontalZoom") {
        const secondsPerPixel = initialTimelineViewport.secondsPerPixel;
        return { ...view, secondsPerPixel, beginSeconds: view.beginSeconds + anchor * (view.secondsPerPixel - secondsPerPixel) };
    }
    const laneHeight = initialTimelineViewport.laneHeight;
    return { ...view, laneHeight, scrollTop: Math.max(0, (view.scrollTop + anchor) * laneHeight / view.laneHeight - anchor) };
}
export function timelineMiddleDragAction(modifiers: ZoomModifiers): MiddleDrag["action"] {
    return timelineZoomAction(modifiers) ?? "pan";
}
const middleDragZoomSensitivity = 3;
export function middleZoomPickup(view: TimelineViewport, point: Point, cursor: Point): MiddleZoomPickup {
    return { point, cursor, timeSeconds: view.beginSeconds + point.x * view.secondsPerPixel,
        lanePosition: (view.scrollTop + point.y) / view.laneHeight };
}

export function zoomMiddleDragAtPickup(view: TimelineViewport, delta: Point, pickup: MiddleZoomPickup,
    axes: { horizontal: boolean; vertical: boolean }): TimelineViewport {
    const { point, timeSeconds, lanePosition } = pickup;
    let next = view;
    if (axes.horizontal && delta.x) {
        next = zoomTimelineHorizontally(next, point.x, delta.x * middleDragZoomSensitivity);
        next.beginSeconds = timeSeconds - point.x * next.secondsPerPixel;
    }
    if (axes.vertical && delta.y) {
        next = zoomTimelineVertically(next, point.y, delta.y * middleDragZoomSensitivity);
        next.scrollTop = Math.max(0, lanePosition * next.laneHeight - point.y);
    }
    return next;
}

export function middleDragZoomFactor(pixels: number): number {
    return zoomFactorForPixels(-pixels * middleDragZoomSensitivity);
}
export function zoomMiddleDrag(view: TimelineViewport, action: TimelineZoomAction, delta: Point, anchor: number): TimelineViewport {
    return action === "verticalZoom"
        ? delta.x ? scaleTimelineLanes(view, anchor, middleDragZoomFactor(delta.x)) : view
        : delta.y ? zoomTimelineHorizontally(view, anchor, delta.y * middleDragZoomSensitivity) : view;
}

export function wrapTimelineCursor(point: Point, travel: Point, bounds: { left: number; top: number; width: number; height: number }) {
    const wrap = (position: number, start: number, size: number) => {
        if (size <= 0) return start;
        const offset = (position - start) % size;
        return start + (offset < 0 ? offset + size : offset);
    };
    const virtual = { x: point.x + travel.x, y: point.y + travel.y };
    const visible = { x: wrap(virtual.x, bounds.left, bounds.width), y: wrap(virtual.y, bounds.top, bounds.height) };
    const screens = {
        x: bounds.width > 0 ? Math.floor((virtual.x - bounds.left) / bounds.width) : 0,
        y: bounds.height > 0 ? Math.floor((virtual.y - bounds.top) / bounds.height) : 0,
    };
    return { virtual, visible, screens };
}

export function zoomAnchorHint(action: TimelineZoomAction | "valueZoom", screens: Point): string {
    const timeAxis = action === "horizontalZoom";
    const name = timeAxis ? "Time anchor" : action === "valueZoom" ? "Value anchor" : "Lane anchor";
    const offset = timeAxis ? screens.x : screens.y;
    if (offset === 0) return `${name} · in view`;
    const distance = Math.abs(offset);
    const direction = timeAxis ? (offset < 0 ? "left" : "right") : (offset < 0 ? "above" : "below");
    return `${name} · ${distance} ${distance === 1 ? "screen" : "screens"} ${direction}`;
}

export function moveMiddleDrag(view: TimelineViewport, drag: SingleMiddleDrag, buttons: number, x: number, y: number,
    anchor: Point, lanePansVertically = false): TimelineViewport | null {
    if (!(buttons & 4)) return null;
    const delta = { x: x - drag.x, y: y - drag.y };
    if (drag.action === "pan") return panTimeline(view, delta, lanePansVertically);
    return zoomMiddleDrag(view, drag.action, delta, drag.action === "horizontalZoom" ? anchor.x : anchor.y);
}

export function panTimeline(view: TimelineViewport, delta: Point, lanePansVertically: boolean): TimelineViewport {
    const verticalTravel = lanePansVertically ? 0 : delta.y;
    if (delta.x === 0 && verticalTravel === 0) return view;
    return { ...view, beginSeconds: view.beginSeconds - delta.x * view.secondsPerPixel,
        scrollTop: Math.max(0, view.scrollTop - verticalTravel) };
}

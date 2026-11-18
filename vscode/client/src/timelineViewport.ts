import type { Point } from "./graphScope";

export type TimelineViewport = {
    beginSeconds: number;
    secondsPerPixel: number;
    laneHeight: number;
    scrollTop: number;
};
export const initialTimelineViewport: TimelineViewport = {
    beginSeconds: 0, secondsPerPixel: 0.01, laneHeight: 240, scrollTop: 0,
};
export const minimumLaneHeight = 12;
export const maximumLaneHeight = 1600;
const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

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
export type TimelineWheelAction = "verticalScroll" | "horizontalScroll" | "laneValueZoom" | TimelineZoomAction;
type ZoomModifiers = { ctrlKey: boolean; altKey: boolean };
function timelineZoomAction(modifiers: ZoomModifiers): TimelineZoomAction | null {
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
    return Math.exp(clamp(pixels * .001, -20, 20));
}

export function zoomTimelineHorizontally(view: TimelineViewport, pointerX: number, delta: number): TimelineViewport {
    const secondsPerPixel = clamp(view.secondsPerPixel * zoomFactorForPixels(delta), 1e-6, 86400);
    return { ...view, secondsPerPixel,
        beginSeconds: view.beginSeconds + pointerX * (view.secondsPerPixel - secondsPerPixel) };
}

export function zoomTimelineVertically(view: TimelineViewport, pointerY: number, delta: number): TimelineViewport {
    const laneHeight = clamp(view.laneHeight * zoomFactorForPixels(-delta), minimumLaneHeight, maximumLaneHeight);
    const lanePosition = (view.scrollTop + pointerY) / view.laneHeight;
    return { ...view, laneHeight, scrollTop: Math.max(0, lanePosition * laneHeight - pointerY) };
}

export type MiddleDrag = { pointerId: number; x: number; y: number; action: "pan" | TimelineZoomAction };
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
export function middleDragZoomFactor(delta: number): number {
    return zoomFactorForPixels(delta * middleDragZoomSensitivity);
}
export function zoomMiddleDrag(view: TimelineViewport, action: TimelineZoomAction, delta: Point, anchor: number): TimelineViewport {
    return action === "verticalZoom"
        ? delta.x ? zoomTimelineVertically(view, anchor, delta.x * middleDragZoomSensitivity) : view
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

export function moveMiddleDrag(view: TimelineViewport, drag: MiddleDrag, buttons: number, x: number, y: number,
    anchor: number, lanePansVertically = false): TimelineViewport | null {
    if (!(buttons & 4)) return null;
    const delta = { x: x - drag.x, y: y - drag.y };
    return drag.action === "pan" ? panTimeline(view, delta, lanePansVertically) : zoomMiddleDrag(view, drag.action, delta, anchor);
}

export function panTimeline(view: TimelineViewport, delta: Point, lanePansVertically: boolean): TimelineViewport {
    const verticalTravel = lanePansVertically ? 0 : delta.y;
    if (delta.x === 0 && verticalTravel === 0) return view;
    return { ...view, beginSeconds: view.beginSeconds - delta.x * view.secondsPerPixel,
        scrollTop: Math.max(0, view.scrollTop - verticalTravel) };
}

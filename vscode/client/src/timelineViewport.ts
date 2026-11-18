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
export type TimelineWheelAction = "verticalScroll" | "horizontalScroll" | TimelineZoomAction;
type ZoomModifiers = { ctrlKey: boolean; altKey: boolean };
function timelineZoomAction(modifiers: ZoomModifiers): TimelineZoomAction | null {
    return modifiers.ctrlKey ? "horizontalZoom" : modifiers.altKey ? "verticalZoom" : null;
}
export function timelineWheelAction(modifiers: ZoomModifiers & { shiftKey: boolean }): TimelineWheelAction {
    return timelineZoomAction(modifiers) ?? (modifiers.shiftKey ? "horizontalScroll" : "verticalScroll");
}

export function wheelPixels(delta: number, mode: number, pageSize: number): number {
    return delta * (mode === 1 ? 16 : mode === 2 ? pageSize : 1);
}

export function zoomTimelineHorizontally(view: TimelineViewport, pointerX: number, delta: number): TimelineViewport {
    const secondsPerPixel = clamp(view.secondsPerPixel * Math.exp(clamp(delta * 0.001, -20, 20)), 1e-6, 86400);
    return { ...view, secondsPerPixel,
        beginSeconds: view.beginSeconds + pointerX * (view.secondsPerPixel - secondsPerPixel) };
}

export function zoomTimelineVertically(view: TimelineViewport, pointerY: number, delta: number): TimelineViewport {
    const laneHeight = clamp(view.laneHeight * Math.exp(clamp(-delta * 0.001, -20, 20)), minimumLaneHeight, maximumLaneHeight);
    const lanePosition = (view.scrollTop + pointerY) / view.laneHeight;
    return { ...view, laneHeight, scrollTop: Math.max(0, lanePosition * laneHeight - pointerY) };
}

export type MiddleDrag = { pointerId: number; x: number; y: number; action: "pan" | TimelineZoomAction };
export function timelineMiddleDragAction(modifiers: ZoomModifiers): MiddleDrag["action"] {
    return timelineZoomAction(modifiers) ?? "pan";
}
const middleDragZoomSensitivity = 3;
export function moveMiddleDrag(view: TimelineViewport, drag: MiddleDrag, buttons: number, x: number, y: number,
    anchor: number): TimelineViewport | null {
    if (!(buttons & 4)) return null;
    const dx = x - drag.x;
    const dy = y - drag.y;
    if (drag.action === "verticalZoom") return dx ? zoomTimelineVertically(view, anchor, -dx * middleDragZoomSensitivity) : view;
    if (drag.action === "horizontalZoom") return dy ? zoomTimelineHorizontally(view, anchor, dy * middleDragZoomSensitivity) : view;
    if (dx === 0 && dy === 0) return view;
    return { ...view, beginSeconds: view.beginSeconds - dx * view.secondsPerPixel,
        scrollTop: Math.max(0, view.scrollTop - dy) };
}

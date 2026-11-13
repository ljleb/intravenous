export type TimelineViewport = {
    beginSeconds: number;
    secondsPerPixel: number;
    laneHeight: number;
    scrollTop: number;
};
export const initialTimelineViewport: TimelineViewport = {
    beginSeconds: 0, secondsPerPixel: 0.01, laneHeight: 240, scrollTop: 0,
};
export const minimumLaneHeight = 32;
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

export type TimelineWheelAction = "verticalScroll" | "verticalZoom" | "horizontalScroll" | "horizontalZoom";
export function timelineWheelAction(modifiers: { ctrlKey: boolean; altKey: boolean; shiftKey: boolean }): TimelineWheelAction {
    if (modifiers.ctrlKey) return "horizontalZoom";
    if (modifiers.altKey) return "verticalZoom";
    if (modifiers.shiftKey) return "horizontalScroll";
    return "verticalScroll";
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

export type MiddlePan = { pointerId: number; x: number; y: number; view: TimelineViewport };
export function moveMiddlePan(pan: MiddlePan, buttons: number, x: number, y: number): TimelineViewport | null {
    if (!(buttons & 4)) return null;
    return { ...pan.view, beginSeconds: pan.view.beginSeconds - (x - pan.x) * pan.view.secondsPerPixel,
        scrollTop: Math.max(0, pan.view.scrollTop - (y - pan.y)) };
}

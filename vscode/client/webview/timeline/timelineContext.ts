import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { TimelineGridMode } from "../../src/timelineSnap";

export type PresentationTimeWindow = { beginSeconds: number; endSeconds: number; spanSeconds?: number; sampleRate?: number | null;
    gridMode?: TimelineGridMode };
const timeWindowKey: InjectionKey<Readonly<Ref<PresentationTimeWindow>>> = Symbol("iv.timelineWindow");
export function provideTimelineWindow(window: Readonly<Ref<PresentationTimeWindow>>): void { provide(timeWindowKey, window); }
export function useTimelineWindow(): Readonly<Ref<PresentationTimeWindow>> | null { return inject(timeWindowKey, null); }

export type TimelineMark = { index: bigint; depth: number; x: number };
const marksKey: InjectionKey<Readonly<Ref<readonly TimelineMark[]>>> = Symbol("iv.timelineMarks");
export function provideTimelineMarks(marks: Readonly<Ref<readonly TimelineMark[]>>): void { provide(marksKey, marks); }
export function useTimelineMarks(): Readonly<Ref<readonly TimelineMark[]>> | null { return inject(marksKey, null); }

export type LaneValueGesture = { content: Element; changed: () => boolean } & (
    { mode: "pan"; pan: (deltaY: number) => void }
    | { mode: "zoom"; zoom: (factor: number, cursorY: number) => void }
);
export type LaneHorizontalZoomGesture = { content: Element; changed: () => boolean; zoom: (factor: number, cursorX: number) => void };
export type LaneValueNavigation = {
    begin: (mode: "pan" | "zoom") => LaneValueGesture | null;
    beginHorizontalZoom?: () => LaneHorizontalZoomGesture | null;
    zoom: (factor: number, cursorY: number) => boolean;
    reset: () => void;
};
type TimelineNavigation = {
    wheel: (event: WheelEvent) => void;
    beginMiddleDrag: (event: PointerEvent) => void;
    registerValueNavigation: (element: Element, lane: LaneValueNavigation) => () => void;
};
const navigationKey: InjectionKey<TimelineNavigation> = Symbol("iv.timelineNavigation");
export function provideTimelineNavigation(navigation: TimelineNavigation): void { provide(navigationKey, navigation); }
export function useTimelineNavigation(): TimelineNavigation | null { return inject(navigationKey, null); }

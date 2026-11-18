import { inject, provide, type InjectionKey, type Ref } from "vue";

export type PresentationTimeWindow = { beginSeconds: number; endSeconds: number; spanSeconds?: number; sampleRate?: number | null };
const timeWindowKey: InjectionKey<Readonly<Ref<PresentationTimeWindow>>> = Symbol("iv.timelineWindow");
export function provideTimelineWindow(window: Readonly<Ref<PresentationTimeWindow>>): void { provide(timeWindowKey, window); }
export function useTimelineWindow(): Readonly<Ref<PresentationTimeWindow>> | null { return inject(timeWindowKey, null); }

export type LaneValuePan = { move: (dy: number) => void; changed: () => boolean };
export type LaneValueNavigation = { begin: () => LaneValuePan | null; reset: () => void };
type TimelineNavigation = {
    wheel: (event: WheelEvent) => void;
    beginMiddleDrag: (event: PointerEvent) => void;
    registerValueNavigation: (element: Element, lane: LaneValueNavigation) => () => void;
};
const navigationKey: InjectionKey<TimelineNavigation> = Symbol("iv.timelineNavigation");
export function provideTimelineNavigation(navigation: TimelineNavigation): void { provide(navigationKey, navigation); }
export function useTimelineNavigation(): TimelineNavigation | null { return inject(navigationKey, null); }

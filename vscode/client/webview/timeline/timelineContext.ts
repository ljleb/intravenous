import { inject, provide, type InjectionKey, type Ref } from "vue";

export type PresentationTimeWindow = { beginSeconds: number; endSeconds: number; sampleRate?: number | null };
const timeWindowKey: InjectionKey<Readonly<Ref<PresentationTimeWindow>>> = Symbol("iv.timelineWindow");
export function provideTimelineWindow(window: Readonly<Ref<PresentationTimeWindow>>): void { provide(timeWindowKey, window); }
export function useTimelineWindow(): Readonly<Ref<PresentationTimeWindow>> | null { return inject(timeWindowKey, null); }

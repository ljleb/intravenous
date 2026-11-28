import { inject, provide, type InjectionKey, type Ref } from "vue";

// Presentations may use the standard lane frame or supply their own layout.
// Host actions do not require the presentation to render a timeline.
export type LaneContext = {
    copyDebugDetails: () => void;
    openSource?: () => void;
    rename?: (name: string, previousName: string) => void;
    regionHeight: Readonly<Ref<number | undefined>>;
    icon?: Readonly<Ref<string | undefined>>;
};
const key: InjectionKey<LaneContext | null> = Symbol("nodeLane");
export function provideLane(context: LaneContext | null): void { provide(key, context); }
export function useLane(): LaneContext | null { return inject(key, null); }

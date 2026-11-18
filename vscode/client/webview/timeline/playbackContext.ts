import { computed, inject, provide, shallowRef, type InjectionKey, type Ref, type ShallowRef } from "vue";
import { PlaybackDisplay, type PlaybackSnapshot } from "../../src/playback";

type PlaybackPresentation = {
    state: ShallowRef<PlaybackSnapshot | null>;
    backgroundPlayhead: Readonly<Ref<bigint | null>>;
    foregroundPlayhead: ShallowRef<bigint | null>;
    requestedPosition: ShallowRef<bigint | null>;
    dragPosition: ShallowRef<string | null>;
};
const key: InjectionKey<PlaybackPresentation> = Symbol("project playback");
export function providePlayback() {
    const state = shallowRef<PlaybackSnapshot | null>(null);
    const appliedBackgroundPlayhead = shallowRef<bigint | null>(null);
    const foregroundPlayhead = shallowRef<bigint | null>(null);
    const requestedPosition = shallowRef<bigint | null>(null);
    const dragPosition = shallowRef<string | null>(null);
    const backgroundPlayhead = computed(() => dragPosition.value === null ? appliedBackgroundPlayhead.value : BigInt(dragPosition.value));
    const display = new PlaybackDisplay();
    let frame = 0;
    const animate = () => {
        const now = performance.now();
        appliedBackgroundPlayhead.value = display.backgroundPlayhead(now);
        foregroundPlayhead.value = display.foregroundPlayhead(now);
        frame = display.isAnimating(now) ? requestAnimationFrame(animate) : 0;
    };
    provide(key, { state, backgroundPlayhead, foregroundPlayhead, requestedPosition, dragPosition });
    return {
        dragPosition,
        receive(snapshot: PlaybackSnapshot | null, requested: string | null) {
            cancelAnimationFrame(frame);
            display.update(snapshot, performance.now());
            state.value = snapshot;
            requestedPosition.value = requested === null ? null : BigInt(requested);
            animate();
        },
        dispose() { cancelAnimationFrame(frame); },
    };
}
export function usePlayback(): PlaybackPresentation {
    return inject(key, { state: shallowRef(null), backgroundPlayhead: shallowRef(null), foregroundPlayhead: shallowRef(null),
        requestedPosition: shallowRef(null), dragPosition: shallowRef(null) });
}

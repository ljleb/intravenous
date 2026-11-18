import { computed, inject, provide, shallowRef, type InjectionKey, type Ref, type ShallowRef } from "vue";
import { PlaybackDisplay, type PlaybackSnapshot } from "../../src/playback";

type PlaybackPresentation = {
    state: ShallowRef<PlaybackSnapshot | null>;
    position: Readonly<Ref<bigint | null>>;
    executionIndex: ShallowRef<bigint | null>;
    requestedPosition: ShallowRef<bigint | null>;
    dragPosition: ShallowRef<string | null>;
};
const key: InjectionKey<PlaybackPresentation> = Symbol("project playback");
export function providePlayback() {
    const state = shallowRef<PlaybackSnapshot | null>(null);
    const appliedPosition = shallowRef<bigint | null>(null);
    const executionIndex = shallowRef<bigint | null>(null);
    const requestedPosition = shallowRef<bigint | null>(null);
    const dragPosition = shallowRef<string | null>(null);
    const position = computed(() => dragPosition.value === null ? appliedPosition.value : BigInt(dragPosition.value));
    const display = new PlaybackDisplay();
    let frame = 0;
    const animate = () => {
        const now = performance.now();
        appliedPosition.value = display.position(now);
        executionIndex.value = display.executionIndex(now);
        frame = display.isAnimating(now) ? requestAnimationFrame(animate) : 0;
    };
    provide(key, { state, position, executionIndex, requestedPosition, dragPosition });
    return {
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
    return inject(key, { state: shallowRef(null), position: shallowRef(null), executionIndex: shallowRef(null),
        requestedPosition: shallowRef(null), dragPosition: shallowRef(null) });
}

import { inject, provide, shallowRef, type InjectionKey, type ShallowRef } from "vue";
import { PlaybackDisplay, type PlaybackSnapshot } from "../../src/playback";

type PlaybackPresentation = {
    state: ShallowRef<PlaybackSnapshot | null>;
    position: ShallowRef<bigint | null>;
    requestedPosition: ShallowRef<bigint | null>;
};
const key: InjectionKey<PlaybackPresentation> = Symbol("project playback");
export function providePlayback() {
    const state = shallowRef<PlaybackSnapshot | null>(null);
    const position = shallowRef<bigint | null>(null);
    const requestedPosition = shallowRef<bigint | null>(null);
    const display = new PlaybackDisplay();
    let frame = 0;
    const animate = () => {
        position.value = display.position(performance.now());
        frame = state.value?.running ? requestAnimationFrame(animate) : 0;
    };
    provide(key, { state, position, requestedPosition });
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
    return inject(key, { state: shallowRef(null), position: shallowRef(null), requestedPosition: shallowRef(null) });
}

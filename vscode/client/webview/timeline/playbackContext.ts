import { computed, inject, provide, shallowRef, type InjectionKey, type Ref, type ShallowRef } from "vue";
import { PlaybackDisplay, type PlaybackSnapshot } from "../../src/playback";
import type { GraphEditorHostMessage } from "../../src/graphEditorProtocol";
import { usePresentationFrame } from "../presentation/presentationFrames";

type PlaybackPresentation = {
    state: ShallowRef<PlaybackSnapshot | null>;
    backgroundPlayhead: Readonly<Ref<bigint | null>>;
    foregroundPlayhead: Readonly<Ref<bigint | null>>;
    requestedPosition: ShallowRef<bigint | null>;
    dragPosition: ShallowRef<string | null>;
    dragPreview: Readonly<Ref<bigint | null>>;
    releaseDrag: (position: string) => string;
};
const key: InjectionKey<PlaybackPresentation> = Symbol("project playback");
export function providePlayback() {
    const state = shallowRef<PlaybackSnapshot | null>(null);
    const appliedBackgroundPlayhead = shallowRef<bigint | null>(null);
    const appliedForegroundPlayhead = shallowRef<bigint | null>(null);
    const requestedPosition = shallowRef<bigint | null>(null);
    const dragPosition = shallowRef<string | null>(null);
    const localRelease = shallowRef<{ id: string; position: bigint } | null>(null);
    const pendingRelease = shallowRef<bigint | null>(null);
    const dragPreview = computed(() => dragPosition.value === null
        ? localRelease.value?.position ?? pendingRelease.value : BigInt(dragPosition.value));
    const backgroundPlayhead = computed(() => dragPreview.value ?? appliedBackgroundPlayhead.value);
    const foregroundPlayhead = computed(() => state.value?.foregroundRunning === false
        ? backgroundPlayhead.value : appliedForegroundPlayhead.value);
    const display = new PlaybackDisplay();
    const animate = () => {
        const now = performance.now();
        appliedBackgroundPlayhead.value = display.backgroundPlayhead(now);
        appliedForegroundPlayhead.value = display.foregroundPlayhead(now);
        if (display.isAnimating(now)) frame.schedule();
    };
    const frame = usePresentationFrame("update", animate);
    provide(key, { state, backgroundPlayhead, foregroundPlayhead, requestedPosition, dragPosition, dragPreview,
        releaseDrag(position) {
            const id = crypto.randomUUID();
            localRelease.value = { id, position: BigInt(position) };
            dragPosition.value = null;
            return id;
        } });
    return {
        dragPosition,
        receive({ snapshot, requestedPosition: requested, releasedDragPosition: released, dragReleaseId: releaseId }
            : Extract<GraphEditorHostMessage, { type: "playbackUpdate" }>) {
            frame.cancel();
            display.update(snapshot, performance.now());
            state.value = snapshot;
            requestedPosition.value = requested === null ? null : BigInt(requested);
            pendingRelease.value = released === null ? null : BigInt(released);
            if (localRelease.value?.id === releaseId) localRelease.value = null;
            animate();
        },
        dispose: frame.cancel,
    };
}
export function usePlayback(): PlaybackPresentation {
    return inject(key, { state: shallowRef(null), backgroundPlayhead: shallowRef(null), foregroundPlayhead: shallowRef(null),
        requestedPosition: shallowRef(null), dragPosition: shallowRef(null), dragPreview: shallowRef(null), releaseDrag: () => "" });
}

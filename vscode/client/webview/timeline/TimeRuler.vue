<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { timelineTicks, type TimelineWindow } from "./geometry";
import RulerGraduations from "./RulerGraduations.vue";
import { usePlayback } from "./playbackContext";
import { playbackSampleIndexFromSeconds } from "../../src/playback";
import { webviewApi } from "../host";

// A presentation can use a time ruler without being a graph or a lane.
// It makes no assumption about the node's number or kinds of output ports.
const props = defineProps<{ window: TimelineWindow; playback?: boolean; sampleRate?: number }>();
const { position, state, requestedPosition } = usePlayback();
const sampleRate = computed(() => props.sampleRate ?? state.value?.sampleRate ?? 48000);
const root = ref<HTMLElement | null>(null);
const width = ref(0);
const ticks = computed(() => timelineTicks(props.window, width.value));
const markerPosition = (selected: bigint | null) => selected === null ? -1
    : (Number(selected) / sampleRate.value - props.window.beginSeconds)
        * width.value / (props.window.endSeconds - props.window.beginSeconds);
const marker = computed(() => markerPosition(position.value));
const requestedMarker = computed(() => markerPosition(requestedPosition.value));
function seek(event: PointerEvent) {
    if (!props.playback || event.button !== 0 || !root.value) return;
    const bounds = root.value.getBoundingClientRect();
    const seconds = props.window.beginSeconds + (event.clientX - bounds.left) / Math.max(1, bounds.width)
        * (props.window.endSeconds - props.window.beginSeconds);
    try { webviewApi.postMessage({ type: "playbackSeek", position: playbackSampleIndexFromSeconds(seconds, sampleRate.value) }); }
    catch { return; }
    event.preventDefault();
    event.stopPropagation();
}
let observer: ResizeObserver | null = null;
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        const entry = entries[entries.length - 1];
        if (entry) width.value = entry.contentRect.width;
    });
    if (root.value) observer.observe(root.value);
});
onBeforeUnmount(() => observer?.disconnect());
</script>

<template>
    <div ref="root" class="time-ruler" aria-label="Time in seconds" @pointerdown="seek"
        :title="playback ? 'Click to seek. Space: play/pause.' : undefined">
        <RulerGraduations :ticks="ticks" />
        <div v-if="playback && marker >= 0 && marker <= width" class="playhead-marker" :style="{ left: `${marker}px` }" />
        <div v-if="playback && requestedMarker >= 0 && requestedMarker <= width" class="requested-marker"
            :style="{ left: `${requestedMarker}px` }" aria-label="Requested playhead position" />
    </div>
</template>

<style scoped>
.time-ruler { position: relative; min-width: 0; overflow: hidden; }
.playhead-marker { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--vscode-debugIcon-startForeground, #89d185); pointer-events: none; }
.requested-marker { position: absolute; top: 0; bottom: 0; border-left: 1px dashed var(--vscode-focusBorder, #569cd6); pointer-events: none; }
</style>

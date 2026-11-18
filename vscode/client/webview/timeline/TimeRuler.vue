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
const { position, executionIndex, state, requestedPosition, dragPosition } = usePlayback();
const sampleRate = computed(() => props.sampleRate ?? state.value?.sampleRate ?? 48000);
const root = ref<HTMLElement | null>(null);
const width = ref(0);
const ticks = computed(() => timelineTicks(props.window, width.value));
const markerPosition = (selected: bigint | null) => selected === null ? -1
    : (Number(selected) / sampleRate.value - props.window.beginSeconds)
        * width.value / (props.window.endSeconds - props.window.beginSeconds);
const marker = computed(() => markerPosition(position.value));
const executionMarker = computed(() => markerPosition(executionIndex.value));
const displayedStart = computed(() => dragPosition.value ?? state.value?.startPosition);
const startMarker = computed(() => {
    const start = displayedStart.value;
    return markerPosition(start === undefined ? null : BigInt(start));
});
const startLabel = computed(() => `Selected start at sample ${displayedStart.value}`);
const requestedMarker = computed(() => markerPosition(dragPosition.value === null ? requestedPosition.value : null));
let pointer: { id: number; x: number; position: string } | null = null;
let postedPosition: string | null = null;
let seekFrame = 0;

function positionAt(clientX: number) {
    if (!root.value) return null;
    const bounds = root.value.getBoundingClientRect();
    const seconds = props.window.beginSeconds + (clientX - bounds.left) / Math.max(1, bounds.width)
        * (props.window.endSeconds - props.window.beginSeconds);
    try { return playbackSampleIndexFromSeconds(seconds, sampleRate.value); }
    catch { return null; }
}

function postDragMove() {
    seekFrame = 0;
    const position = dragPosition.value;
    if (position === null || position === postedPosition) return;
    webviewApi.postMessage({ type: "playbackDrag", phase: "move", position });
    postedPosition = position;
}

function beginSeek(event: PointerEvent) {
    if (!props.playback || event.button !== 0 || pointer !== null || !root.value) return;
    const position = positionAt(event.clientX);
    if (position === null) return;
    root.value.setPointerCapture(event.pointerId);
    pointer = { id: event.pointerId, x: event.clientX, position };
    event.preventDefault();
    event.stopPropagation();
}

function moveSeek(event: PointerEvent) {
    if (!pointer || event.pointerId !== pointer.id) return;
    if (!(event.buttons & 1)) { finishSeek(); return; }
    const position = positionAt(event.clientX);
    if (position === null || dragPosition.value === position) return;
    if (dragPosition.value === null) {
        if (event.clientX === pointer.x) return;
        dragPosition.value = position;
        postedPosition = position;
        webviewApi.postMessage({ type: "playbackDrag", phase: "begin", position });
        return;
    }
    dragPosition.value = position;
    if (!seekFrame) seekFrame = requestAnimationFrame(postDragMove);
}

function finishSeek(position?: string) {
    if (!pointer) return;
    const held = dragPosition.value;
    const pointerId = pointer.id;
    cancelAnimationFrame(seekFrame);
    seekFrame = 0;
    pointer = null;
    postedPosition = null;
    dragPosition.value = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (held !== null) webviewApi.postMessage({ type: "playbackDrag", phase: "end", position: position ?? held });
    else if (position !== undefined) webviewApi.postMessage({ type: "playbackSeek", position });
}

function endSeek(event: PointerEvent) {
    if (!pointer || event.pointerId !== pointer.id) return;
    finishSeek(positionAt(event.clientX) ?? dragPosition.value ?? pointer.position);
    event.stopPropagation();
}

function cancelSeek(event: PointerEvent) {
    if (event.pointerId === pointer?.id) finishSeek();
}
function visibilityChanged() {
    if (document.hidden) finishSeek();
}
let observer: ResizeObserver | null = null;
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        const entry = entries[entries.length - 1];
        if (entry) width.value = entry.contentRect.width;
    });
    if (root.value) observer.observe(root.value);
    if (props.playback) document.addEventListener("visibilitychange", visibilityChanged);
});
onBeforeUnmount(() => {
    finishSeek();
    document.removeEventListener("visibilitychange", visibilityChanged);
    observer?.disconnect();
});
</script>

<template>
    <div ref="root" class="time-ruler" :class="{ seekable: playback }" aria-label="Time in seconds"
        @pointerdown="beginSeek" @pointermove="moveSeek" @pointerup="endSeek"
        @pointercancel="cancelSeek" @lostpointercapture="cancelSeek"
        :data-status-shortcuts="playback ? 'Click/drag seek · Space ▶/Ⅱ' : undefined">
        <RulerGraduations :ticks="ticks" />
        <div v-if="playback && executionMarker >= 0 && executionMarker <= width && executionMarker !== marker"
            class="playhead-marker execution-marker" :style="{ left: `${executionMarker}px` }"
            role="img" aria-label="Foreground Tick position" />
        <div v-if="playback && marker >= 0 && marker <= width" class="playhead-marker" :style="{ left: `${marker}px` }" />
        <div v-if="playback && requestedMarker >= 0 && requestedMarker <= width" class="requested-marker"
            :style="{ left: `${requestedMarker}px` }" aria-label="Requested playhead position" />
        <div v-if="playback && startMarker >= 0 && startMarker <= width" class="start-marker"
            :style="{ left: `${startMarker}px` }" role="img" :aria-label="startLabel" :data-status-hint="`Start ${displayedStart} smp`" />
    </div>
</template>

<style scoped>
.time-ruler { position: relative; min-width: 0; overflow: hidden; }
.seekable { cursor: ew-resize; touch-action: none; user-select: none; }
.playhead-marker { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--vscode-debugIcon-startForeground, #89d185); pointer-events: none; }
.execution-marker { background: var(--iv-ruler-tick); }
.requested-marker { position: absolute; top: 0; bottom: 0; border-left: 1px dashed var(--vscode-focusBorder, #569cd6); pointer-events: none; }
.start-marker {
    position: absolute; bottom: 0; margin-left: -5px;
    /* The tip shares the cursor's one-pixel column. */
    box-sizing: content-box; width: 1px; height: 0;
    border-inline: 5px solid transparent; border-bottom: 7px solid var(--vscode-foreground);
}
</style>

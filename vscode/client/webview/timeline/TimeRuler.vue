<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { snapTimelineSample, timelineTicks } from "./geometry";
import RulerGraduations from "./RulerGraduations.vue";
import TimelinePlayhead from "./TimelinePlayhead.vue";
import type { PresentationTimeWindow, TimelineMark } from "./timelineContext";
import { usePlayback } from "./playbackContext";
import { playbackSampleIndexFromSeconds } from "../../src/playback";
import { webviewApi } from "../host";

// A presentation can use a time ruler without being a graph or a lane.
// It makes no assumption about the node's number or kinds of output ports.
const props = defineProps<{ window: PresentationTimeWindow; marks?: readonly TimelineMark[]; playback?: boolean }>();
const { state, requestedPosition, dragPosition } = usePlayback();
const sampleRate = computed(() => state.value?.sampleRate ?? props.window.sampleRate ?? 48000);
const root = ref<HTMLElement | null>(null);
const width = ref(0);
const ticks = computed(() => timelineTicks(props.window, width.value));
const markerPosition = (selected: bigint | null) => selected === null ? -1
    : (Number(selected) / sampleRate.value - props.window.beginSeconds)
        * width.value / (props.window.endSeconds - props.window.beginSeconds);
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
    try {
        const index = playbackSampleIndexFromSeconds(seconds, sampleRate.value);
        const tolerance = 6 * sampleRate.value * (props.window.endSeconds - props.window.beginSeconds) / Math.max(1, bounds.width);
        return snapTimelineSample(BigInt(index), props.marks ?? [], tolerance).toString();
    }
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
        <svg v-if="playback || marks?.length" class="ruler-playheads">
            <line v-for="mark in marks" :key="mark.index.toString()" class="beat-mark"
                :x1="mark.x" :x2="mark.x" :y1="mark.depth === 0 ? 12 : mark.depth === 1 ? 20 : 27" y2="100%" />

            <TimelinePlayhead v-if="playback" :window="window" />
            <line v-if="requestedMarker >= 0 && requestedMarker <= width" class="requested-marker"
                :x1="requestedMarker" :x2="requestedMarker" y1="0" y2="100%"
                role="img" aria-label="Requested background playhead position" />
            <polygon v-if="startMarker >= 0 && startMarker <= width" class="start-marker" points="-5.5,0 5.5,0 0,7"
                :style="{ transform: `translateX(${startMarker}px)` }" role="img" :aria-label="startLabel"
                :data-status-hint="`Start ${displayedStart} smp`" />
        </svg>
    </div>
</template>

<style scoped>
.time-ruler { position: relative; min-width: 0; overflow: hidden; }
.seekable { cursor: ew-resize; touch-action: none; user-select: none; }
.ruler-playheads { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.beat-mark { stroke: var(--vscode-editorRuler-foreground, #777); opacity: .75; }
.requested-marker { stroke: var(--vscode-focusBorder, #569cd6); stroke-dasharray: 3 3; }
.start-marker { fill: var(--iv-background-playhead); pointer-events: auto; }
</style>

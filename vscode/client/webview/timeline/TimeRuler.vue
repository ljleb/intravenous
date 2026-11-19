<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { rulerMarkerPoints, timelineSampleFraction, timelineTicks } from "./geometry";
import RulerGraduations from "./RulerGraduations.vue";
import TimelinePlayhead from "./TimelinePlayhead.vue";
import type { PresentationTimeWindow, TimelineMark } from "./timelineContext";
import { usePlayback } from "./playbackContext";
import { usePlayheadSeek } from "./usePlayheadSeek";

// A presentation can use a time ruler without being a graph or a lane.
// It makes no assumption about the node's number or kinds of output ports.
const props = defineProps<{ window: PresentationTimeWindow; marks?: readonly TimelineMark[]; playback?: boolean }>();
const { state, requestedPosition, dragPreview } = usePlayback();
const sampleRate = computed(() => state.value?.sampleRate ?? props.window.sampleRate ?? 48000);
const root = ref<HTMLElement | null>(null);
const width = ref(0);
const showBeats = computed(() => props.window.gridMode === "beats");
const ticks = computed(() => timelineTicks(props.window, width.value));
const beginSeek = usePlayheadSeek(root, () => props.playback ? props.window : undefined,
    () => showBeats.value ? props.marks?.map(({ index }) => index) ?? [] : []);
const markerPosition = (selected: bigint | null) => selected === null ? -1
    : timelineSampleFraction(selected, props.window, sampleRate.value) * width.value;
const displayedStart = computed(() => dragPreview.value?.toString() ?? state.value?.startPosition);
const startMarker = computed(() => {
    const start = displayedStart.value;
    return markerPosition(start === undefined ? null : BigInt(start));
});
const startLabel = computed(() => `Selected start at sample ${displayedStart.value}`);
const requestedMarker = computed(() => markerPosition(dragPreview.value === null ? requestedPosition.value : null));
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
    <div ref="root" class="time-ruler" :class="{ seekable: playback }" :aria-label="showBeats ? 'Beat grid' : 'Time in seconds'"
        @pointerdown="beginSeek"
        :data-status-shortcuts="playback ? 'Click/drag seek · Space ▶/Ⅱ' : undefined">
        <RulerGraduations v-if="!showBeats" :ticks="ticks" />
        <svg v-if="playback || marks?.length" class="ruler-playheads">
            <line v-for="mark in showBeats ? marks : []" :key="mark.index.toString()" class="beat-mark"
                :x1="mark.x" :x2="mark.x" :y1="mark.depth === 0 ? 12 : mark.depth === 1 ? 20 : 27" y2="100%" />

            <TimelinePlayhead v-if="playback" :window="window" ruler />
            <line v-if="requestedMarker >= 0 && requestedMarker <= width" class="requested-marker"
                :x1="requestedMarker" :x2="requestedMarker" y1="0" y2="100%"
                role="img" aria-label="Requested background playhead position" />
            <polygon v-if="startMarker >= 0 && startMarker <= width" class="start-marker" :points="rulerMarkerPoints"
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

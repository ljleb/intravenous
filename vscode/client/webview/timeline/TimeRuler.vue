<script setup lang="ts">
import { computed, ref } from "vue";
import type { BeatRulerLabel } from "../../src/beatGrid";
import { rulerMarkerPoints, timelineSampleFraction, timelineTicks } from "./geometry";
import RulerGraduations from "./RulerGraduations.vue";
import GridGuides from "./GridGuides.vue";
import { usePresentationSize } from "../presentation/presentationSize";
import TimelinePlayhead from "./TimelinePlayhead.vue";
import { timelineMarkKey, timelineMarkPosition, type PresentationTimeWindow, type TimelineMark } from "./timelineContext";
import { usePlayback } from "./playbackContext";
import { usePlayheadSeek, type SeekBeatReader } from "./usePlayheadSeek";

// A presentation can use a time ruler without being a graph or a lane.
// It makes no assumption about the node's number or kinds of output ports.
const props = defineProps<{ window: PresentationTimeWindow; marks?: readonly TimelineMark[];
    beatLabels?: readonly BeatRulerLabel[];
    beatStrengths?: ReadonlyMap<number, number>; playback?: boolean; openBeatReader?: () => SeekBeatReader | undefined }>();
const { state, requestedPosition, dragPreview } = usePlayback();
const sampleRate = computed(() => state.value?.sampleRate ?? props.window.sampleRate ?? 48000);
const root = ref<HTMLElement | null>(null);
const size = usePresentationSize(root);
const width = computed(() => size.value?.width ?? 0);
const showBeats = computed(() => props.window.gridMode === "beats" || props.window.gridMode === "custom");
const beatLineStart = ({ depth }: TimelineMark) => depth === 0 ? "33%" : depth === 1 ? "56%" : depth === 2 ? "75%" : "83%";
const ticks = computed(() => timelineTicks(props.window, width.value));
const beginSeek = usePlayheadSeek(root, () => props.playback ? props.window : undefined,
    () => showBeats.value ? props.marks?.map(({ index }) => index) ?? [] : [],
    () => showBeats.value ? props.openBeatReader?.() : undefined);
const markerPosition = (selected: bigint | null) => selected === null ? -1
    : timelineSampleFraction(selected, props.window, sampleRate.value) * width.value;
const displayedStart = computed(() => dragPreview.value?.toString() ?? state.value?.startPosition);
const startMarker = computed(() => {
    const start = displayedStart.value;
    return markerPosition(start === undefined ? null : BigInt(start));
});
const startLabel = computed(() => `Selected start at sample ${displayedStart.value}`);
const requestedMarker = computed(() => markerPosition(dragPreview.value === null ? requestedPosition.value : null));
</script>

<template>
    <div ref="root" class="time-ruler" :class="{ seekable: playback }" :aria-label="showBeats ? 'Beat grid' : 'Time in seconds'"
        @pointerdown="beginSeek"
        :data-status-shortcuts="playback ? 'Click/drag seek · Space ▶/Ⅱ' : undefined">
        <RulerGraduations v-if="!showBeats" :ticks="ticks" />
        <svg v-if="playback || marks?.length" class="ruler-playheads">
            <GridGuides v-if="showBeats && marks" :marks="marks" :strengths="beatStrengths"
                :mark-key="timelineMarkKey" :position="timelineMarkPosition" :start="beatLineStart" />
            <text v-for="label in showBeats ? beatLabels : []" :key="label.text" class="beat-label"
                :x="label.x + 4" y="11">{{ label.text }}</text>

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
.ruler-playheads { --iv-grid-max-opacity: .84; }
.beat-label { fill: var(--vscode-descriptionForeground); font: 10px monospace; }
.requested-marker { stroke: var(--vscode-focusBorder, #569cd6); stroke-dasharray: 3 3; }
.start-marker { fill: var(--iv-background-playhead); pointer-events: auto; }
</style>

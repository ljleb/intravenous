<script setup lang="ts">
import { computed } from "vue";
import { rulerPlayheadPath, timelineSampleFraction } from "../../src/timelineGeometry";
import { usePlayback } from "./playbackContext";
import { useTimelineWindow, type PresentationTimeWindow } from "./timelineContext";

const props = defineProps<{ window?: PresentationTimeWindow; rulerHeight?: number }>();
const playback = usePlayback();
const timeline = useTimelineWindow();
function markerX(position: bigint | null) {
    const window = props.window ?? timeline?.value;
    if (position === null || !window) return null;

    const fraction = timelineSampleFraction(position, window);
    return fraction >= 0 && fraction <= 1 ? `${fraction * 100}%` : null;
}
const backgroundX = computed(() => markerX(playback.backgroundPlayhead.value));
const foregroundX = computed(() => markerX(playback.foregroundPlayhead.value));
const rulerPath = computed(() => props.rulerHeight === undefined ? undefined : rulerPlayheadPath(props.rulerHeight));
</script>

<template>
    <template v-if="foregroundX !== null && foregroundX !== backgroundX">
        <svg v-if="rulerPath" :x="foregroundX" width="1" height="100%" overflow="visible" class="timeline-playhead foreground-playhead"
            role="img" aria-label="Foreground playhead">
            <path :d="rulerPath" />
        </svg>
        <line v-else :x1="foregroundX" :x2="foregroundX" y1="0" y2="100%" class="timeline-playhead foreground-playhead"
            vector-effect="non-scaling-stroke" role="img" aria-label="Foreground playhead" />
    </template>
    <template v-if="backgroundX !== null">
        <svg v-if="rulerPath" :x="backgroundX" width="1" height="100%" overflow="visible"
            class="timeline-playhead background-playhead" role="img" aria-label="Background playhead">
            <path :d="rulerPath" />
        </svg>
        <line v-else :x1="backgroundX" :x2="backgroundX" y1="0" y2="100%" class="timeline-playhead background-playhead"
            vector-effect="non-scaling-stroke" role="img" aria-label="Background playhead" />
    </template>
</template>

<style scoped>
.timeline-playhead { pointer-events: none; }
.background-playhead { color: var(--iv-background-playhead); opacity: .65; }
.foreground-playhead { color: var(--iv-lane-ruler-tick); }
line.timeline-playhead { stroke: currentColor; }
.timeline-playhead path { fill: currentColor; }
</style>

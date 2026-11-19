<script setup lang="ts">
import { computed } from "vue";
import type { ObservationWindow } from "../../src/observations";
import { rulerMarkerPoints } from "./geometry";
import { usePlayback } from "./playbackContext";
import { useTimelineWindow, type PresentationTimeWindow } from "./timelineContext";

const props = defineProps<{ window?: PresentationTimeWindow; sampleWindow?: ObservationWindow; ruler?: boolean }>();
const playback = usePlayback();
const timeline = useTimelineWindow();
function markerX(position: bigint | null) {
    const window = props.window ?? timeline?.value;
    if (position === null || !window) return null;

    let fraction: number;
    if (props.sampleWindow) {
        const offset = BigInt.asUintN(64, position - BigInt(props.sampleWindow.begin));
        fraction = Number(offset) / Number(props.sampleWindow.length);
    } else {
        const rate = playback.state.value?.sampleRate ?? window.sampleRate;
        if (!rate) return null;
        fraction = (Number(position) / rate - window.beginSeconds) / (window.endSeconds - window.beginSeconds);
    }
    return fraction >= 0 && fraction <= 1 ? `${fraction * 100}%` : null;
}
const backgroundX = computed(() => markerX(playback.backgroundPlayhead.value));
const foregroundX = computed(() => markerX(playback.foregroundPlayhead.value));
</script>

<template>
    <g v-if="foregroundX !== null && foregroundX !== backgroundX" class="timeline-playhead foreground-playhead"
        role="img" aria-label="Foreground playhead">
        <line :x1="foregroundX" :x2="foregroundX" y1="0" y2="100%" vector-effect="non-scaling-stroke" />
        <svg v-if="ruler" :x="foregroundX" width="1" height="7" overflow="visible" aria-hidden="true">
            <polygon :points="rulerMarkerPoints" />
        </svg>
    </g>
    <line v-if="backgroundX !== null" class="timeline-playhead background-playhead" :x1="backgroundX" :x2="backgroundX" y1="0" y2="100%"
        vector-effect="non-scaling-stroke" role="img" aria-label="Background playhead" />
</template>

<style scoped>
.timeline-playhead { pointer-events: none; }
.background-playhead { stroke: var(--iv-background-playhead); stroke-opacity: .65; }
.foreground-playhead { color: var(--iv-lane-ruler-tick); }
.foreground-playhead line { stroke: currentColor; }
.foreground-playhead polygon { fill: currentColor; }
</style>

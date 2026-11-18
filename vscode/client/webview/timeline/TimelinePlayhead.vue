<script setup lang="ts">
import { computed } from "vue";
import type { ObservationWindow } from "../../src/observations";
import { usePlayback } from "./playbackContext";
import { useTimelineWindow } from "./timelineContext";

const props = defineProps<{ sampleWindow?: ObservationWindow }>();
const playback = usePlayback();
const timeline = useTimelineWindow();
function markerX(position: bigint | null) {
    const window = timeline?.value;
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
const x = computed(() => markerX(playback.position.value));
const executionX = computed(() => markerX(playback.executionIndex.value));
</script>

<template>
    <line v-if="executionX !== null && executionX !== x" class="timeline-playhead execution-playhead"
        :x1="executionX" :x2="executionX" y1="0" y2="100%" vector-effect="non-scaling-stroke" aria-hidden="true" />
    <line v-if="x !== null" class="timeline-playhead" :x1="x" :x2="x" y1="0" y2="100%"
        vector-effect="non-scaling-stroke" aria-hidden="true" />
</template>

<style scoped>
.timeline-playhead { stroke: var(--vscode-debugIcon-startForeground, #89d185); stroke-opacity: .65; pointer-events: none; }
.execution-playhead { stroke: var(--iv-lane-ruler-tick); stroke-opacity: 1; }
</style>

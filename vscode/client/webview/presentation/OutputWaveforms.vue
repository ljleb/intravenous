<script setup lang="ts">
import { computed } from "vue";
import { addressKey } from "../../src/graphScope";
import { sampleWaveform, sampleWindow } from "../../src/sampleWaveform";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import { useTimelineWindow } from "../timeline/timelineContext";
import { useNode } from "./nodeContext";
import { useObservations } from "./observationContext";
const props = defineProps<{ bounds?: { min: number; max: number }; mode?: string; inputMember?: string }>();
const node = useNode();
const observations = useObservations();
const timeline = useTimelineWindow();
const visibleWindow = computed(() => {
    const time = timeline?.value;
    return time ? sampleWindow(time.beginSeconds, time.endSeconds, time.sampleRate, 2) : undefined;
});
const waveforms = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => entry.delivery === "window"
        && (props.inputMember === undefined ? entry.kind === "output" : entry.kind === "input" && entry.member === props.inputMember)
        && addressKey(entry.source.node) === addressKey(address)).map((entry) => {
        const result = observations.values.windowForDisplay(entry.id, visibleWindow.value);
        const label = entry.kind === "input" ? `Input ${entry.member}` : entry.member;
        return { ...entry, label, result, geometry: sampleWaveform(result, props.bounds, visibleWindow.value) };
    });
});
function waveformStatus(item: (typeof waveforms.value)[number]): string {
    const label = [item.label, item.channelName].filter(Boolean).join(" · ");
    const detail = item.result?.error || (item.result?.outdated ? "Updating" : item.result?.truncated ? "Zoom in for full window"
        : item.result?.status === "ready" ? `Approx. · ${props.bounds?.min ?? -1}…${props.bounds?.max ?? 1}`
            : item.result?.status ?? "pending");
    return `${label} · ${detail}`;
}
</script>

<template>
    <div v-if="waveforms.length" class="window-outputs" :data-mode="mode">
        <div v-for="item in waveforms" :key="item.id" class="waveform"
            :class="{ unavailable: item.result?.status !== 'ready' || item.result?.outdated, failed: item.result?.status === 'failed' }"
            :data-status-hint="waveformStatus(item)">
            <svg viewBox="0 0 1000 100" preserveAspectRatio="none" role="img"
                :aria-label="`${item.label}: ${item.result?.status ?? 'pending'} representative waveform`">
                <rect v-for="(region, i) in item.geometry.coverage" :key="`coverage-${i}`"
                    :x="region.x" y="4" :width="region.width" height="92" class="coverage" />
                <path v-for="(path, i) in item.geometry.paths" :key="`path-${i}`" :d="path"
                    fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" vector-effect="non-scaling-stroke" />
                <TimelinePlayhead :sample-window="visibleWindow ?? item.result?.window" />
            </svg>
            <span class="label">{{ item.label }}<template v-if="item.channels && item.channels.length > 1"> · {{ item.channelName }}</template></span>
        </div>
    </div>
</template>

<style scoped>
.window-outputs { display: flex; flex-direction: column; flex: 1; min-height: 1px; }
.waveform { position: relative; flex: 1; min-height: 1px; color: #62bf7b; overflow: hidden; }
.waveform svg { display: block; width: 100%; height: 100%; min-height: 1px; }
.coverage { fill: currentColor; opacity: .045; }
.label { position: absolute; top: 2px; left: 8px; font-size: 10px; color: var(--vscode-descriptionForeground); pointer-events: none; }
[data-mode="nano"] .label, [data-mode="nano"] path { display: none; }
[data-mode="nano"] .coverage { opacity: .2; }
.unavailable { opacity: .45; }
.failed .label { color: var(--vscode-errorForeground); }
</style>

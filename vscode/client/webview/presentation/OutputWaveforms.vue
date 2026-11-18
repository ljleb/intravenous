<script setup lang="ts">
import { computed } from "vue";
import { addressKey } from "../../src/graphScope";
import { sampleWaveform } from "../../src/sampleWaveform";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import { useNode } from "./nodeContext";
import { useObservations } from "./observationContext";
const props = defineProps<{ bounds?: { min: number; max: number }; mode?: string }>();
const node = useNode();
const observations = useObservations();
const outputs = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => entry.delivery === "window"
        && addressKey(entry.source.node) === addressKey(address)).map((entry) => {
        const result = observations.values.windows.get(entry.id);
        return { ...entry, result, geometry: sampleWaveform(result, props.bounds) };
    });
});
function waveformStatus(item: (typeof outputs.value)[number]): string {
    const label = [item.member, item.channelName].filter(Boolean).join(" · ");
    const detail = item.result?.error || (item.result?.outdated ? "Updating" : item.result?.truncated ? "Zoom in for full window"
        : item.result?.status === "ready" ? `Approx. · ${props.bounds?.min ?? -1}…${props.bounds?.max ?? 1}`
            : item.result?.status ?? "pending");
    return `${label} · ${detail}`;
}
</script>

<template>
    <div v-if="outputs.length" class="window-outputs" :data-mode="mode">
        <div v-for="output in outputs" :key="output.id" class="waveform"
            :class="{ unavailable: output.result?.status !== 'ready' || output.result?.outdated, failed: output.result?.status === 'failed' }"
            :data-status-hint="waveformStatus(output)">
            <svg viewBox="0 0 1000 100" preserveAspectRatio="none" role="img"
                :aria-label="`${output.member}: ${output.result?.status ?? 'pending'} representative waveform`">
                <rect v-for="(region, i) in output.geometry.coverage" :key="`coverage-${i}`"
                    :x="region.x" y="4" :width="region.width" height="92" class="coverage" />
                <path v-for="(path, i) in output.geometry.paths" :key="`path-${i}`" :d="path"
                    fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" vector-effect="non-scaling-stroke" />
                <TimelinePlayhead :sample-window="output.result?.window" />
            </svg>
            <span class="label">{{ output.member }}<template v-if="output.channels && output.channels.length > 1"> · {{ output.channelName }}</template></span>
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

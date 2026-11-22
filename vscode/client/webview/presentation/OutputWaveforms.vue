<script setup lang="ts">
import { computed } from "vue";
import { sampleWaveform } from "../../src/sampleWaveform";
import type { ObservationWindow } from "../../src/observations";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import { useNode } from "./nodeContext";
import { useObservations, useObservationTier } from "./observationContext";
import { useTimelineWindow } from "../timeline/timelineContext";
import { timelineObservationOffset, timelineSamples } from "../../src/timelineSamples";
const props = defineProps<{ bounds?: { min: number; max: number }; mode?: string; inputMember?: string; window?: ObservationWindow }>();
const node = useNode();
const observations = useObservations();
const tier = useObservationTier();
const time = useTimelineWindow();
const waveforms = computed(() => {
    const address = node.value.address;
    const visible = props.window;
    if (tier.value !== "visible" || !address || !observations || !visible || !time) return [];
    const view = timelineSamples(time.value);
    const visibleOffset = timelineObservationOffset(view);
    return observations.forNode(address).filter((entry) => entry.delivery === "window"
        && (props.inputMember === undefined ? entry.kind === "output" : entry.kind === "input" && entry.member === props.inputMember)).map((entry) => {
        const { result, spans, status, error } = observations.viewportWindow(entry.id, visible, visible, false);
        const layers = spans.flatMap(({ result, begin, end, windowOffset }) => {
            const scale = 1000 / view.span;
            const from = Math.max(0, (Number(begin) + visibleOffset) * scale);
            const to = Math.min(1000, (Number(end) + visibleOffset) * scale);
            return to > from ? [{ x: from, width: to - from,
                geometry: sampleWaveform(result, props.bounds, view, windowOffset) }] : [];
        });
        const port = node.value.ports.find((port) => port.direction === entry.kind && port.name === entry.member);
        const label = port?.displayName ?? (entry.kind === "input" ? `Input ${entry.member}` : entry.member);
        return { ...entry, label, result, layers, status, error };
    });
});
function waveformStatus(item: (typeof waveforms.value)[number]): string {
    const label = [item.label, item.channelName].filter(Boolean).join(" · ");
    const detail = item.error || (item.result?.outdated || (item.result?.status === "ready" && item.status === "pending")
        ? "Updating" : item.result?.truncated ? "Zoom in for full window"
        : item.result?.status === "ready" ? `Approx. · ${props.bounds?.min ?? -1}…${props.bounds?.max ?? 1}`
            : item.result?.status ?? "pending");
    return `${label} · ${detail}`;
}
</script>

<template>
    <div v-if="waveforms.length" class="window-outputs" :data-mode="mode">
        <div v-for="item in waveforms" :key="item.id" class="waveform"
            :class="{ unavailable: item.result?.status !== 'ready' || item.result?.outdated,
                failed: item.status === 'failed' || !!item.error }"
            :data-status-hint="waveformStatus(item)">
            <svg viewBox="0 0 1000 100" preserveAspectRatio="none" role="img"
                :aria-label="`${item.label}: ${item.result?.status ?? 'pending'} representative waveform`">
                <svg v-for="layer in item.layers" :key="layer.x" :x="layer.x" :width="layer.width" height="100"
                    :viewBox="`${layer.x} 0 ${layer.width} 100`" preserveAspectRatio="none" overflow="hidden">
                    <rect v-for="(region, i) in layer.geometry.coverage" :key="`coverage-${i}`"
                        :x="region.x" y="4" :width="region.width" height="92" class="coverage" />
                    <path v-for="(path, i) in layer.geometry.paths" :key="`path-${i}`" :d="path"
                        fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" vector-effect="non-scaling-stroke" />
                </svg>
                <TimelinePlayhead />
            </svg>
            <span class="label">{{ item.label }}<template v-if="item.channels && item.channels.length > 1"> · {{ item.channelName }}</template></span>
        </div>
    </div>
</template>

<style scoped>
.window-outputs { display: flex; flex-direction: column; flex: 1; min-height: 1px; }
.waveform { position: relative; flex: 1; min-height: 1px; color: #62bf7b; overflow: hidden; }
.waveform > svg { display: block; width: 100%; height: 100%; min-height: 1px; }
.coverage { fill: currentColor; opacity: .045; }
.label { position: absolute; top: 2px; left: 8px; font-size: 10px; color: var(--vscode-descriptionForeground); pointer-events: none; }
[data-mode="nano"] .label, [data-mode="nano"] path { display: none; }
[data-mode="nano"] .coverage { opacity: .2; }
.unavailable { opacity: .45; }
.failed .label { color: var(--vscode-errorForeground); }
</style>

<script setup lang="ts">
import { computed, onBeforeUnmount } from "vue";
import { addressKey } from "../../src/graphScope";
import { sampleWaveform } from "../../src/sampleWaveform";
import { useNode } from "./nodeContext";
import { useObservations } from "./observationContext";
const props = defineProps<{ bounds?: { min: number; max: number }; mode?: string }>();
const node = useNode();
const observations = useObservations();
const owner = Symbol("waveform status");
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
function hint(id: string | null): void {
    const item = outputs.value.find((entry) => entry.id === id);
    observations?.hint(owner, item ? {
        text: `${item.member}${item.channelName ? ` · ${item.channelName}` : ""} · representative waveform · ${item.result?.outdated ? "updating" : item.result?.status ?? "pending"}`,
        tooltip: item.result?.error || (item.result?.truncated
            ? "The bounded result is incomplete. Zoom in to request a smaller window."
            : "Approximate position samples. Gaps remain empty; peaks between samples may be missed. Vertical scale: −1 to 1."),
    } : null, 2);
}
onBeforeUnmount(() => observations?.hint(owner, null));
</script>

<template>
    <div v-if="outputs.length" class="window-outputs" :data-mode="mode">
        <div v-for="output in outputs" :key="output.id" class="waveform"
            :class="{ unavailable: output.result?.status !== 'ready' || output.result?.outdated, failed: output.result?.status === 'failed' }"
            @pointerenter="hint(output.id)" @pointerleave="hint(null)">
            <svg viewBox="0 0 1000 100" preserveAspectRatio="none" role="img"
                :aria-label="`${output.member}: ${output.result?.status ?? 'pending'} representative waveform`">
                <rect v-for="(region, i) in output.geometry.coverage" :key="`coverage-${i}`"
                    :x="region.x" y="4" :width="region.width" height="92" class="coverage" />
                <path v-for="(path, i) in output.geometry.paths" :key="`path-${i}`" :d="path"
                    fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" vector-effect="non-scaling-stroke" />
            </svg>
            <span class="label">{{ output.member }}<template v-if="output.channels && output.channels.length > 1"> · {{ output.channelName }}</template></span>
        </div>
    </div>
</template>

<style scoped>
.window-outputs { display: flex; flex-direction: column; flex: 1; min-height: 1px; margin: 0 -8px; }
.waveform { position: relative; flex: 1; min-height: 1px; color: #62bf7b; overflow: hidden; }
.waveform svg { display: block; width: 100%; height: 100%; min-height: 1px; }
.coverage { fill: currentColor; opacity: .045; }
.label { position: absolute; top: 2px; left: 8px; font-size: 10px; color: var(--vscode-descriptionForeground); pointer-events: none; }
[data-mode="nano"] .label, [data-mode="nano"] path { display: none; }
[data-mode="nano"] .coverage { opacity: .2; }
.unavailable { opacity: .45; }
.failed .label { color: var(--vscode-errorForeground); }
</style>

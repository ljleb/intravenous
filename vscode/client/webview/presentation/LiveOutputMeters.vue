<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useNode } from "./nodeContext";
import { useObservations, type PresentedObservation } from "./observationContext";
import SampleMeter from "./SampleMeter.vue";
import OutputWaveforms from "./OutputWaveforms.vue";
import { useTimelineWindow } from "../timeline/timelineContext";
import { sampleWindow } from "../../src/sampleWaveform";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import type { MeterMode } from "../../src/observations";
import type { NodeDisplayMode } from "./displayMode";

const props = defineProps<{ mode?: NodeDisplayMode; bounds?: { min: number; max: number } }>();

const node = useNode();
const observations = useObservations();
const root = ref<HTMLElement | null>(null);
const owner = Symbol("live output presentation");
let observer: IntersectionObserver | undefined;
let resize: ResizeObserver | undefined;
const timeline = useTimelineWindow();
const width = ref(0);
const window = computed(() => timeline ? sampleWindow(timeline.value.beginSeconds,
    timeline.value.endSeconds, timeline.value.sampleRate, props.mode === "nano" ? 2 : width.value) : undefined);
let intersecting = false;
const visible = ref(false);
let current: NodeAddress | undefined;
function updateVisibility(): void {
    visible.value = !!current && intersecting && !document.hidden;
    if (current) observations?.node(owner, current, visible.value, window.value);
}
watch(() => node.value.address ? addressKey(node.value.address) : null, () => {
    observations?.remove(owner);
    current = node.value.address;
    updateVisibility();
}, { immediate: true });
watch(window, updateVisibility);
const items = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => entry.delivery !== "window"
        && addressKey(entry.source.node) === addressKey(address))
        .map((entry) => ({ ...entry, result: observations.values.values.get(entry.id) }));
});
const outputs = computed(() => {
    const groups = new Map<string, { member: string; mode: MeterMode; operations: string[]; channels: typeof items.value }>();
    for (const item of items.value) {
        if (item.kind !== "output") continue;
        let group = groups.get(item.member);
        if (!group) {
            group = { member: item.member, mode: observations?.mode(item.source.node, item.member) ?? "level", operations: item.operations, channels: [] };
            groups.set(item.member, group);
        }
        group.channels.push(item);
    }
    return [...groups.values()];
});
const state = computed(() => items.value.filter((item) => item.kind === "stateField"));
function switchMode(member: string, mode: MeterMode): void {
    if (current) observations?.setMode(current, member, mode);
    modeHint(member, mode, true);
}
function channelLabel(item: PresentedObservation): string {
    return item.channels && item.channels.length > 1 ? `${item.member} · ${item.channelName}` : item.member;
}
function modeHint(member: string, mode: MeterMode, active: boolean, priority = 2): void {
    observations?.hint(owner, active ? {
        text: `${member} · ${mode} meter · click to switch`,
        tooltip: "Both scales show sample peak and RMS. Level uses dBFS; value uses a linear magnitude scale.",
    } : null, priority);
}
function stateText(value: unknown): string {
    return typeof value === "number" && Number.isFinite(value) ? String(value) : "—";
}
onMounted(() => {
    document.addEventListener("visibilitychange", updateVisibility);
    observer = new IntersectionObserver(([entry]) => {
        intersecting = !!entry?.isIntersecting;
        updateVisibility();
    }, { rootMargin: "100px 0px" });
    if (root.value) {
        observer.observe(root.value);
        resize = new ResizeObserver(([entry]) => { width.value = entry?.contentRect.width ?? 0; });
        resize.observe(root.value);
    }
});
onBeforeUnmount(() => {
    document.removeEventListener("visibilitychange", updateVisibility);
    observer?.disconnect();
    resize?.disconnect();
    observations?.remove(owner);
    observations?.hint(owner, null);
});
</script>

<template>
    <div ref="root" class="live-outputs" :data-mode="mode" :aria-description="observations?.error.value || undefined">
        <div v-for="output in outputs" :key="output.member" class="output-group">
            <div class="output-label">
                <span class="output-name">{{ output.member }}</span>
                <button v-if="output.operations.includes('level')" type="button"
                    :aria-label="`${output.member}: ${output.mode} meter. Switch to ${output.mode === 'level' ? 'value' : 'level'}`"
                    @pointerenter="modeHint(output.member, output.mode, true)" @pointerleave="modeHint(output.member, output.mode, false)"
                    @focus="modeHint(output.member, output.mode, true, 1)" @blur="modeHint(output.member, output.mode, false)"
                    @click.stop="switchMode(output.member, output.mode === 'level' ? 'value' : 'level')">
                    {{ output.mode === 'level' ? 'level · dBFS' : 'value' }}
                </button>
            </div>
            <div class="output-channels">
                <div v-for="item in output.channels" :key="JSON.stringify([node.definitionId, item.id])" class="channel">
                    <SampleMeter :mode="output.mode" :label="channelLabel(item)" :result="item.result" :active="visible" />
                </div>
            </div>
        </div>
        <OutputWaveforms :mode="mode" :bounds="bounds" />
        <div v-for="item in state" :key="item.id" class="state-field">
            <span class="output-name">{{ item.member }}</span>
            <output :aria-label="item.member">{{ item.result?.available ? stateText(item.result.value?.value) : '—' }}</output>
        </div>
    </div>
</template>

<style scoped>
.live-outputs { height: 100%; min-height: 0; overflow: hidden; display: flex; flex-direction: column; gap: 4px; padding: 0 8px; box-sizing: border-box; }
.output-group { display: flex; flex: 1 1 0; align-items: stretch; gap: 8px; min-height: 0; overflow: hidden; }
.output-label { display: flex; flex-direction: column; justify-content: center; align-items: flex-start; gap: 2px; flex: 0 1 90px; min-width: 35px; overflow: hidden; }
.output-name { max-width: 100%; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; color: var(--vscode-descriptionForeground); font-size: 11px; }
.output-channels { display: flex; flex: 1; min-width: 0; flex-direction: column; gap: 2px; min-height: 0; }
.channel { display: flex; flex: 1; align-items: stretch; gap: 5px; min-height: 0; min-width: 0; }
.state-field { display: flex; align-items: center; gap: 8px; font-size: 11px; }
output { font-variant-numeric: tabular-nums; }
[data-mode="nano"] .state-field { display: none; }
[data-mode="nano"] .output-label { flex-direction: row; align-items: center; }
[data-mode="nano"] .output-label .output-name { display: none; }
button { border: 0; padding: 1px 3px; color: var(--vscode-descriptionForeground); background: transparent; font-size: 10px; white-space: nowrap; cursor: pointer; }
button:hover { color: var(--vscode-foreground); background: var(--vscode-toolbar-hoverBackground); }
button:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
@container node-lane (max-height: 40px) {
    .live-outputs { gap: 0; }
    .output-label { flex-direction: row; align-items: center; }
    .output-label .output-name, .state-field { display: none; }
    .output-label button { font-size: min(10px, calc(100cqh - 4px)); line-height: 1; padding-top: 0; padding-bottom: 0; }
    .output-channels { gap: 1px; }
}
</style>

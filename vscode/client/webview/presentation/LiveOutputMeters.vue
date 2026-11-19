<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useNode } from "./nodeContext";
import { useObservations, type PresentedObservation } from "./observationContext";
import SampleMeter from "./SampleMeter.vue";
import OutputWaveforms from "./OutputWaveforms.vue";
import AnchoredPopover from "./AnchoredPopover.vue";
import ContextMenuItem from "./ContextMenuItem.vue";
import { useTimelineWindow } from "../timeline/timelineContext";
import { sampleWindow } from "../../src/sampleWaveform";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { observationSourceKey, type MeterMode, type ObservationSource } from "../../src/observations";
import type { NodeDisplayMode } from "./displayMode";
import { isContextMenuKey, useContextPopover } from "./contextPopover";

const props = defineProps<{ mode?: NodeDisplayMode; bounds?: { min: number; max: number } }>();
const showScaleLabels = computed(() => props.mode === "editable" || props.mode === undefined);

const node = useNode();
const firstInput = computed(() => node.value.ports.some((port) => port.direction === "output") ? undefined
    : node.value.ports.find((port) => port.direction === "input" && port.kind === "continuous")?.name);
const observations = useObservations();
const root = ref<HTMLElement | null>(null);
const owner = Symbol("live port presentation");
const visualizationModes: Array<{ mode: MeterMode; label: string }> = [
    { mode: "level", label: "Level (dBFS)" }, { mode: "value", label: "Value (linear)" },
];
const { panel: menuRoot, key: menuKey, open: openContext, close: closeMenu, toggled: menuToggled } = useContextPopover<string>();
let observer: IntersectionObserver | undefined;
let resize: ResizeObserver | undefined;
const timeline = useTimelineWindow();
const width = ref(0);
const observationWindow = computed(() => timeline ? sampleWindow(timeline.value.beginSeconds,
    timeline.value.endSeconds, timeline.value.sampleRate, props.mode === "nano" ? 2 : Math.min(512, width.value)) : undefined);
let intersecting = false;
const visible = ref(false);
let current: NodeAddress | undefined;
function updateVisibility(): void {
    visible.value = !!current && intersecting && !document.hidden;
    if (current) observations?.node(owner, current, visible.value, observationWindow.value, firstInput.value);
}
watch(() => node.value.address ? addressKey(node.value.address) : null, () => {
    closeMenu();
    observations?.remove(owner);
    current = node.value.address;
    updateVisibility();
}, { immediate: true });
watch([observationWindow, firstInput], updateVisibility);
const items = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => entry.delivery !== "window"
        && addressKey(entry.source.node) === addressKey(address))
        .map((entry) => ({ ...entry, result: observations.values.values.get(entry.id) }));
});
const meters = computed(() => {
    const groups = new Map<string, { key: string; source: ObservationSource; mode: MeterMode; channels: typeof items.value }>();
    for (const item of items.value) {
        if (firstInput.value === undefined ? item.kind !== "output" : item.kind !== "input" || item.member !== firstInput.value) continue;
        const key = observationSourceKey({ ...item.source, channel: undefined });
        let group = groups.get(key);
        if (!group) {
            group = { key, source: item.source, mode: observations?.mode(item.source) ?? "level", channels: [] };
            groups.set(key, group);
        }
        group.channels.push(item);
    }
    return [...groups.values()];
});
const state = computed(() => items.value.filter((item) => item.kind === "stateField"));
const selectedMeter = computed(() => meters.value.find((meter) => meter.key === menuKey.value));
const selectedMode = computed(() => selectedMeter.value?.mode);
watch(selectedMeter, () => {
    if (menuKey.value !== null && !selectedMeter.value) closeMenu();
});
watch(() => node.value.definitionId, () => closeMenu());
function switchMode(mode: MeterMode): void {
    if (selectedMeter.value) observations?.setMode(selectedMeter.value.source, mode);
    closeMenu(true);
}
function channelLabel(item: PresentedObservation): string {
    const name = item.kind === "input" ? `Input ${item.member}` : item.member;
    return item.channels && item.channels.length > 1 ? `${name} · ${item.channelName}` : name;
}
function openMenu(event: MouseEvent | KeyboardEvent, key: string): void {
    if (event instanceof KeyboardEvent && !isContextMenuKey(event)) return;
    const meter = event.target instanceof Element ? event.target.closest<HTMLElement>("[role='meter']") : null;
    openContext(event, key, { anchor: meter, returnFocus: meter === document.activeElement ? meter : null,
        initialFocus: "[aria-checked='true']" });
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
});
</script>

<template>
    <div ref="root" class="live-outputs" :data-mode="mode" :aria-description="observations?.error.value || undefined">
        <div v-for="meter in meters" :key="meter.key" class="output-group"
            @contextmenu="openMenu($event, meter.key)" @keydown="openMenu($event, meter.key)">
            <div class="output-channels">
                <div v-for="item in meter.channels" :key="JSON.stringify([node.definitionId, item.id])" class="channel">
                    <SampleMeter :mode="meter.mode" :label="channelLabel(item)" :result="item.result" :active="visible"
                        :show-scale-labels="showScaleLabels" />
                </div>
            </div>
        </div>
        <OutputWaveforms :mode="mode" :bounds="bounds" :input-member="firstInput" />
        <div v-for="item in state" :key="item.id" class="state-field">
            <span class="output-name">{{ item.member }}</span>
            <output :aria-label="item.member">{{ item.result?.available ? stateText(item.result.value?.value) : '—' }}</output>
        </div>
    </div>
    <Teleport to="body">
        <AnchoredPopover v-if="menuKey !== null" ref="menuRoot" role="menu" label="Visualization mode"
            :data-lane-key="node.address ? addressKey(node.address) : undefined" contextual dismiss-on-viewport-change
            @toggle="menuToggled">
            <ContextMenuItem v-for="choice in visualizationModes" :key="choice.mode" role="menuitemradio"
                :aria-checked="selectedMode === choice.mode"
                @click.stop="switchMode(choice.mode)">
                <template #icon>{{ selectedMode === choice.mode ? '✓' : '' }}</template>
                {{ choice.label }}
            </ContextMenuItem>
        </AnchoredPopover>
    </Teleport>
</template>

<style scoped>
.live-outputs { height: 100%; min-height: 0; overflow: hidden; display: flex; flex-direction: column; gap: 4px; box-sizing: border-box; }
.output-group { display: flex; flex: 1 1 0; align-items: stretch; gap: 8px; min-height: 0; overflow: hidden; }
.output-name { max-width: 100%; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; color: var(--vscode-descriptionForeground); font-size: 11px; }
.output-channels { display: flex; flex: 1; min-width: 0; flex-direction: column; gap: 2px; min-height: 0; }
.channel { display: flex; flex: 1; align-items: stretch; gap: 5px; min-height: 0; min-width: 0; }
.state-field { display: flex; align-items: center; gap: 8px; font-size: 11px; }
output { font-variant-numeric: tabular-nums; }
:is([data-mode="mini"], [data-mode="nano"]).live-outputs { gap: 0; }
:is([data-mode="mini"], [data-mode="nano"]) .state-field { display: none; }
:is([data-mode="mini"], [data-mode="nano"]) .output-channels { gap: 1px; }
</style>

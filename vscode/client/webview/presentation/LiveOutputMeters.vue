<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useNode } from "./nodeContext";
import { useObservations, useObservationTier, type PresentedObservation } from "./observationContext";
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
import { readManualControlValue, useOptionalManualControls } from "./manualControlContext";
import { usePresentationSize } from "./presentationSize";

const props = defineProps<{ mode?: NodeDisplayMode; bounds?: { min: number; max: number } }>();
const showScaleLabels = computed(() => props.mode === "editable" || props.mode === undefined);

const node = useNode();
const firstInput = computed(() => node.value.ports.some((port) => port.direction === "output") ? undefined
    : node.value.ports.find((port) => port.direction === "input" && port.kind === "continuous")?.name);
const observations = useObservations();
const manualControls = useOptionalManualControls();
const outputGains = computed(() => {
    const address = node.value.address;
    const gains = new Map<string, number>();
    if (!address) return gains;
    for (const port of node.value.ports) {
        if (port.direction !== "output" || port.kind !== "continuous" || !port.manualControl?.enabled) continue;
        const gain = readManualControlValue(manualControls, { node: address, outputName: port.name }, port.manualControl);
        if (gain !== undefined) gains.set(port.name, gain);
    }
    return gains;
});
const root = ref<HTMLElement | null>(null);
const owner = Symbol("live port presentation");
const visualizationModes: Array<{ mode: MeterMode; label: string }> = [
    { mode: "level", label: "Level (dBFS)" }, { mode: "value", label: "Value (linear)" },
];
const { panel: menuRoot, key: menuKey, open: openContext, close: closeMenu, toggled: menuToggled } = useContextPopover<string>();
const timeline = useTimelineWindow();
const size = usePresentationSize(root);
const width = computed(() => size.value?.width ?? 0);
const observationWindow = computed(() => timeline ? sampleWindow(timeline.value.beginSeconds,
    timeline.value.endSeconds, timeline.value.sampleRate, props.mode === "nano" ? 2 : Math.min(512, width.value)) : undefined);
const tier = useObservationTier();
const visible = ref(false);
let current: NodeAddress | undefined;
function updateVisibility(): void {
    const active = document.hidden ? undefined : tier.value;
    visible.value = !!current && active === "visible";
    if (current) observations?.node(owner, current, active, observationWindow.value, firstInput.value);
}
watch(() => node.value.address ? addressKey(node.value.address) : null, () => {
    closeMenu();
    observations?.remove(owner);
    current = node.value.address;
    updateVisibility();
}, { immediate: true });
watch([tier, observationWindow, firstInput], updateVisibility);
const items = computed(() => {
    const address = node.value.address;
    if (!visible.value || !address || !observations) return [];
    return observations.forNode(address).filter((entry) => entry.delivery !== "window")
        .map((entry) => {
            const result = observations.values.values.get(entry.id);
            return { ...entry, result, retained: !result || result.pending ? observations.values.previousLiveValue(entry.id) : undefined };
        });
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
    const port = node.value.ports.find((port) => port.direction === item.kind && port.name === item.member);
    const name = port?.displayName ?? (item.kind === "input" ? `Input ${item.member}` : item.member);
    return item.channels && item.channels.length > 1 ? `${name} · ${item.channelName}` : name;
}
function openMenu(event: MouseEvent | KeyboardEvent, key: string): void {
    if (event instanceof KeyboardEvent && !isContextMenuKey(event)) return;
    const meter = event.target instanceof Element ? event.target.closest<HTMLElement>("[role='meter']") : null;
    openContext(event, key, { anchor: meter, returnFocus: meter === document.activeElement ? meter : null,
        initialFocus: "[aria-checked='true']" });
}
onMounted(() => {
    document.addEventListener("visibilitychange", updateVisibility);
});
onBeforeUnmount(() => {
    document.removeEventListener("visibilitychange", updateVisibility);
    observations?.remove(owner);
});
</script>

<template>
    <div ref="root" class="live-outputs" :data-mode="mode" :aria-description="observations?.error.value || undefined">
        <div v-for="meter in meters" :key="meter.key" class="output-group"
            @contextmenu="openMenu($event, meter.key)" @keydown="openMenu($event, meter.key)">
            <div class="output-channels">
                <div v-for="item in meter.channels" :key="JSON.stringify([node.definitionId, item.id])" class="channel">
                    <SampleMeter :mode="meter.mode" :label="channelLabel(item)" :result="item.result" :retained="item.retained" :active="visible"
                        :gain="item.kind === 'output' && !item.gainApplied ? outputGains.get(item.member) : undefined"
                        :show-scale-labels="showScaleLabels" />
                </div>
            </div>
        </div>
        <OutputWaveforms :mode="mode" :bounds="bounds" :input-member="firstInput" :window="observationWindow" />
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
.output-channels { display: flex; flex: 1; min-width: 0; flex-direction: column; gap: 2px; min-height: 0; }
.channel { display: flex; flex: 1; align-items: stretch; gap: 5px; min-height: 0; min-width: 0; }
:is([data-mode="mini"], [data-mode="nano"]).live-outputs { gap: 0; }
:is([data-mode="mini"], [data-mode="nano"]) .output-channels { gap: 1px; }
</style>

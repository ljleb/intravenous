<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
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
const visualizationModes: Array<{ mode: MeterMode; label: string }> = [
    { mode: "level", label: "Level (dBFS)" }, { mode: "value", label: "Value (linear)" },
];
const menu = ref<{ member: string; left: number; top: number; positioned: boolean } | null>(null);
const menuRoot = ref<HTMLElement | null>(null);
let returnFocus: HTMLElement | null = null;
let observer: IntersectionObserver | undefined;
let resize: ResizeObserver | undefined;
const timeline = useTimelineWindow();
const width = ref(0);
const observationWindow = computed(() => timeline ? sampleWindow(timeline.value.beginSeconds,
    timeline.value.endSeconds, timeline.value.sampleRate, props.mode === "nano" ? 2 : width.value) : undefined);
let intersecting = false;
const visible = ref(false);
let current: NodeAddress | undefined;
function updateVisibility(): void {
    visible.value = !!current && intersecting && !document.hidden;
    if (current) observations?.node(owner, current, visible.value, observationWindow.value);
}
watch(() => node.value.address ? addressKey(node.value.address) : null, () => {
    closeMenu();
    observations?.remove(owner);
    current = node.value.address;
    updateVisibility();
}, { immediate: true });
watch(observationWindow, updateVisibility);
const items = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => entry.delivery !== "window"
        && addressKey(entry.source.node) === addressKey(address))
        .map((entry) => ({ ...entry, result: observations.values.values.get(entry.id) }));
});
const outputs = computed(() => {
    const groups = new Map<string, { member: string; mode: MeterMode; channels: typeof items.value }>();
    for (const item of items.value) {
        if (item.kind !== "output") continue;
        let group = groups.get(item.member);
        if (!group) {
            group = { member: item.member, mode: observations?.mode(item.source.node, item.member) ?? "level", channels: [] };
            groups.set(item.member, group);
        }
        group.channels.push(item);
    }
    return [...groups.values()];
});
const state = computed(() => items.value.filter((item) => item.kind === "stateField"));
const selectedMode = computed(() => outputs.value.find((output) => output.member === menu.value?.member)?.mode);
watch(outputs, () => {
    if (menu.value && !outputs.value.some((output) => output.member === menu.value!.member)) closeMenu();
});
watch(() => node.value.definitionId, () => closeMenu());
function switchMode(member: string, mode: MeterMode): void {
    if (current) observations?.setMode(current, member, mode);
    closeMenu(true);
}
function channelLabel(item: PresentedObservation): string {
    return item.channels && item.channels.length > 1 ? `${item.member} · ${item.channelName}` : item.member;
}
function closeMenu(restoreFocus = false): void {
    menu.value = null;
    if (restoreFocus && returnFocus?.isConnected) returnFocus.focus({ preventScroll: true });
    returnFocus = null;
}
async function openMenu(event: MouseEvent | KeyboardEvent, member: string): Promise<void> {
    if (event instanceof KeyboardEvent && event.key !== "ContextMenu" && !(event.shiftKey && event.key === "F10")) return;
    event.preventDefault(); event.stopPropagation();
    returnFocus = event.target instanceof Element ? event.target.closest<HTMLElement>("[role='meter']") : null;
    const rect = returnFocus?.getBoundingClientRect();
    menu.value = { member, left: event instanceof MouseEvent ? event.clientX : rect?.left ?? 0,
        top: event instanceof MouseEvent ? event.clientY : rect?.bottom ?? 0, positioned: false };
    const opened = menu.value;
    await nextTick();
    if (menu.value !== opened || !menuRoot.value) return;
    const size = menuRoot.value.getBoundingClientRect();
    opened.left = Math.max(4, Math.min(opened.left, window.innerWidth - size.width - 4));
    opened.top = Math.max(4, Math.min(opened.top, window.innerHeight - size.height - 4));
    opened.positioned = true;
    await nextTick();
    if (menu.value === opened) menuRoot.value?.querySelector<HTMLElement>("[aria-checked='true']")?.focus({ preventScroll: true });
}
function menuKey(event: KeyboardEvent): void {
    if (event.key === "Escape" || event.key === "Tab") {
        if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); }
        closeMenu(true);
        return;
    }
    const buttons = [...menuRoot.value?.querySelectorAll<HTMLButtonElement>("[role='menuitemradio']") ?? []];
    const index = buttons.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "ArrowDown" ? (index + 1) % buttons.length
        : event.key === "ArrowUp" ? (index + buttons.length - 1) % buttons.length
        : event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1 : -1;
    if (next >= 0) { event.preventDefault(); event.stopPropagation(); buttons[next]?.focus(); }
}
function outsideMenu(event: PointerEvent): void {
    if (!(event.target instanceof Node) || !menuRoot.value?.contains(event.target)) closeMenu();
}
const dismissMenu = () => closeMenu();
function stateText(value: unknown): string {
    return typeof value === "number" && Number.isFinite(value) ? String(value) : "—";
}
onMounted(() => {
    window.addEventListener("pointerdown", outsideMenu, true);
    window.addEventListener("blur", dismissMenu);
    window.addEventListener("resize", dismissMenu);
    window.addEventListener("scroll", dismissMenu, true);
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
    window.removeEventListener("pointerdown", outsideMenu, true);
    window.removeEventListener("blur", dismissMenu);
    window.removeEventListener("resize", dismissMenu);
    window.removeEventListener("scroll", dismissMenu, true);
    document.removeEventListener("visibilitychange", updateVisibility);
    observer?.disconnect();
    resize?.disconnect();
    observations?.remove(owner);
    observations?.hint(owner, null);
});
</script>

<template>
    <div ref="root" class="live-outputs" :data-mode="mode" :aria-description="observations?.error.value || undefined">
        <div v-for="output in outputs" :key="output.member" class="output-group"
            @contextmenu="openMenu($event, output.member)" @keydown="openMenu($event, output.member)">
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
    <Teleport to="body">
        <div v-if="menu" ref="menuRoot" class="visualization-menu" role="menu" aria-label="Visualization mode"
            :class="{ positioning: !menu.positioned }" :aria-hidden="!menu.positioned"
            :style="{ left: `${menu.left}px`, top: `${menu.top}px` }" @keydown="menuKey" @contextmenu.prevent>
            <button v-for="choice in visualizationModes" :key="choice.mode" type="button" role="menuitemradio"
                :aria-checked="selectedMode === choice.mode" :tabindex="selectedMode === choice.mode ? 0 : -1"
                @click.stop="switchMode(menu.member, choice.mode)">
                <span aria-hidden="true">{{ selectedMode === choice.mode ? '✓' : '' }}</span>{{ choice.label }}
            </button>
        </div>
    </Teleport>
</template>

<style scoped>
.live-outputs { height: 100%; min-height: 0; overflow: hidden; display: flex; flex-direction: column; gap: 4px; padding: 0 8px; box-sizing: border-box; }
.output-group { display: flex; flex: 1 1 0; align-items: stretch; gap: 8px; min-height: 0; overflow: hidden; }
.output-name { max-width: 100%; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; color: var(--vscode-descriptionForeground); font-size: 11px; }
.output-channels { display: flex; flex: 1; min-width: 0; flex-direction: column; gap: 2px; min-height: 0; }
.channel { display: flex; flex: 1; align-items: stretch; gap: 5px; min-height: 0; min-width: 0; }
.state-field { display: flex; align-items: center; gap: 8px; font-size: 11px; }
output { font-variant-numeric: tabular-nums; }
[data-mode="nano"] .state-field { display: none; }
.visualization-menu { position: fixed; z-index: 1000; min-width: 160px; padding: 4px; border: 1px solid var(--vscode-menu-border, var(--vscode-widget-border)); border-radius: 4px; background: var(--vscode-menu-background, var(--vscode-editorWidget-background)); color: var(--vscode-menu-foreground, var(--vscode-foreground)); box-shadow: 0 2px 8px var(--vscode-widget-shadow); }
.visualization-menu.positioning { visibility: hidden; pointer-events: none; }
.visualization-menu button { display: flex; align-items: center; gap: 6px; width: 100%; padding: 5px 8px; border: 0; border-radius: 2px; background: transparent; color: inherit; font: inherit; text-align: left; white-space: nowrap; cursor: pointer; }
.visualization-menu button span { width: 12px; }
.visualization-menu button:hover, .visualization-menu button:focus { outline: none; color: var(--vscode-menu-selectionForeground, var(--vscode-list-activeSelectionForeground)); background: var(--vscode-menu-selectionBackground, var(--vscode-list-activeSelectionBackground)); }
@container node-lane (max-height: 40px) {
    .live-outputs { gap: 0; }
    .state-field { display: none; }
    .output-channels { gap: 1px; }
}
</style>

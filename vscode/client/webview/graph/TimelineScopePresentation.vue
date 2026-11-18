<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef } from "vue";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { nodeDisplayName } from "../presentation/nodeSummary";
import TimeRuler from "../timeline/TimeRuler.vue";
import { provideTimelineWheelHandler, provideTimelineWindow } from "../timeline/timelineContext";
import { moveMiddlePan, timelineWheelAction, wheelPixels, zoomTimelineHorizontally,
    zoomTimelineVertically, type MiddlePan } from "../../src/timelineViewport";
import { useGraphEditor } from "./context";
import { usePlayback } from "../timeline/playbackContext";

const node = useNode();
const playback = usePlayback();
const playbackSeconds = computed(() => playback.position.value === null ? null
    : Number(playback.position.value) / (playback.state.value?.sampleRate ?? 48000));
const scope = computed(() => node.value.scope!);
const children = computed(() => scope.value.nodes.map((child) => ({
    child, semantic: semanticNodeFromScopeChild(child, scope.value),
})));
const { layout, save, copyNodeDebugDetails, selectedLane, selectLane } = useGraphEditor();
function clickedLane(event: MouseEvent, address: NodeAddress) {
    if (event.button !== 0 || event.defaultPrevented) return;
    if (event.target instanceof Element
        && event.target.closest("button, input, textarea, select, a, [role='button'], [contenteditable='true']")) return;
    selectLane(address);
}
const viewport = computed({ get: () => layout.value.timeline,
    set: (value) => { layout.value.timeline = value; } });
const laneHeight = computed(() => viewport.value.laneHeight);
const headerHeight = 36;
const selectedLaneIndex = computed(() => scope.value.nodes.findIndex((child) => addressKey(child.address) === selectedLane.value));
const scrollbarStyle = computed(() => {
    const contentHeight = headerHeight + scope.value.nodes.length * laneHeight.value;
    const begin = headerHeight + selectedLaneIndex.value * laneHeight.value;
    return {
        "--timeline-header-height": `${headerHeight}px`,
        "--lane-selection-start": `${100 * begin / contentHeight}%`,
        "--lane-selection-end": `${100 * (begin + laneHeight.value) / contentHeight}%`,
    };
});
const root = ref<HTMLElement | null>(null);
const timeRegion = ref<HTMLElement | null>(null);
const timeWidth = ref(0);
const timeWindow = computed(() => ({ beginSeconds: viewport.value.beginSeconds,
    endSeconds: viewport.value.beginSeconds + Math.max(1, timeWidth.value) * viewport.value.secondsPerPixel,
    sampleRate: scope.value.sampleRate }));
provideTimelineWindow(timeWindow);
provideTimelineWheelHandler(wheel);
const pan = shallowRef<MiddlePan | null>(null);
let panMoved = false;
let observer: ResizeObserver | null = null;
let saveTimer: ReturnType<typeof setTimeout> | undefined;
let scrollRequest = 0;
let disposed = false;

function scheduleSave() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => { saveTimer = undefined; save(); }, 150);
}
async function applyScroll() {
    const request = ++scrollRequest;
    await nextTick();
    if (disposed || request !== scrollRequest || !root.value) return;
    root.value.scrollTop = viewport.value.scrollTop;
    viewport.value.scrollTop = root.value.scrollTop;
}
function scrolled() {
    if (!root.value) return;
    viewport.value.scrollTop = root.value.scrollTop;
    scheduleSave();
}
function wheel(event: WheelEvent) {
    const action = timelineWheelAction(event);
    if (action === "verticalScroll") return; // Native scrolling, including its bounds and momentum.
    event.preventDefault();
    event.stopPropagation();
    if (!root.value || !timeRegion.value) return;
    const axis = timeRegion.value.getBoundingClientRect();
    const delta = wheelPixels(event.deltaY || event.deltaX, event.deltaMode, root.value.clientHeight);
    if (action === "horizontalScroll") {
        const horizontal = Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
        viewport.value = { ...viewport.value, beginSeconds: viewport.value.beginSeconds
            + wheelPixels(horizontal, event.deltaMode, axis.width) * viewport.value.secondsPerPixel };
    } else if (action === "horizontalZoom") {
        viewport.value = zoomTimelineHorizontally(viewport.value,
            Math.max(0, Math.min(axis.width, event.clientX - axis.left)), delta);
    } else {
        const top = root.value.getBoundingClientRect().top + headerHeight;
        viewport.value = zoomTimelineVertically(viewport.value, Math.max(0, event.clientY - top), delta);
        void applyScroll();
    }
    scheduleSave();
}
function preventMiddleDefault(event: MouseEvent) {
    if (event.button === 1) event.preventDefault();
}
function beginPan(event: PointerEvent) {
    if (event.button !== 1 || !root.value) return;
    event.preventDefault();
    pan.value = { pointerId: event.pointerId, x: event.clientX, y: event.clientY,
        view: { ...viewport.value, scrollTop: root.value.scrollTop } };
    panMoved = false;
    root.value.setPointerCapture(event.pointerId);
}
function movePan(event: PointerEvent) {
    if (!pan.value || event.pointerId !== pan.value.pointerId) return;
    const moved = moveMiddlePan({ ...pan.value, view: { ...viewport.value } }, event.buttons, event.clientX, event.clientY);
    if (!moved) { endPan(); return; }
    if (event.clientX === pan.value.x && event.clientY === pan.value.y) return;
    panMoved = true;
    pan.value = { ...pan.value, x: event.clientX, y: event.clientY, view: moved };
    viewport.value = moved;
    void applyScroll();
    scheduleSave();
}
function endPan(event?: PointerEvent) {
    if (!pan.value || (event && event.pointerId !== pan.value.pointerId)) return;
    const pointerId = pan.value.pointerId;
    pan.value = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!panMoved) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
const blurred = () => endPan();
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        for (const entry of entries) if (entry.target === timeRegion.value) timeWidth.value = entry.contentRect.width;
    });
    if (timeRegion.value) observer.observe(timeRegion.value);
    window.addEventListener("blur", blurred);
    void applyScroll();
});
onBeforeUnmount(() => {
    disposed = true;
    observer?.disconnect();
    window.removeEventListener("blur", blurred);
    if (pan.value) endPan();
    if (saveTimer) { clearTimeout(saveTimer); save(); }
});
</script>

<template>
    <section ref="root" class="timeline-scope" :class="{ panning: !!pan }" aria-label="Project timeline"
        :style="scrollbarStyle" :data-has-selection="selectedLaneIndex >= 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown="beginPan" @pointermove="movePan"
        @pointerup="endPan" @pointercancel="endPan" @lostpointercapture="endPan"
        @mousedown="preventMiddleDefault" @auxclick="preventMiddleDefault">
        <header class="timeline-heading">
            <div class="node-column playback-position" data-status-hint="Space ▶/Ⅱ">
                <template v-if="playbackSeconds !== null">
                    {{ playback.state.value?.running ? '▷' : 'Ⅱ' }} {{ playbackSeconds.toFixed(3) }} s
                    <small>{{ playback.position.value?.toString() }} smp</small>
                </template>
            </div>
            <div ref="timeRegion" class="time-region"><TimeRuler :window="timeWindow" :sample-rate="scope.sampleRate" playback style="height: 100%" /></div>
        </header>
        <div class="lanes">
            <section v-for="{ child, semantic } in children" :key="addressKey(child.address)"
                class="lane" :class="{ selected: selectedLane === addressKey(child.address) }"
                :style="{ height: `${laneHeight}px` }" :aria-label="nodeDisplayName(semantic)" tabindex="0"
                :aria-current="selectedLane === addressKey(child.address) ? 'true' : undefined"
                @click="clickedLane($event, child.address)" @keydown.enter.self="selectLane(child.address)">
                <PresentationHost :node="semantic" :region-height="laneHeight - 1" lane
                    @copy-debug-details="copyNodeDebugDetails(child.address)" />
            </section>
            <div v-if="!children.length" class="empty">No project instances.</div>
        </div>
    </section>
</template>

<style scoped>
.timeline-scope { height: 100%; min-height: 0; display: flex; flex-direction: column; overflow-y: auto; scrollbar-gutter: stable; scrollbar-color: auto; scrollbar-width: auto; --node-label-width: clamp(144px, 22%, 260px); }
.timeline-scope::-webkit-scrollbar { width: 14px; height: 10px; }
.timeline-scope::-webkit-scrollbar-track { background-color: var(--vscode-scrollbar-background, transparent); }
/* Paint the overview marker in the native track. It adds no hit target and
   leaves thumb dragging, track clicks and wheel scrolling to the browser. */
.timeline-scope[data-has-selection="true"]::-webkit-scrollbar-track:vertical {
    background-image: linear-gradient(to bottom,
        transparent min(var(--lane-selection-start), calc(100% - 2px)),
        var(--vscode-editorOverviewRuler-selectionHighlightForeground, var(--vscode-focusBorder)) min(var(--lane-selection-start), calc(100% - 2px)),
        var(--vscode-editorOverviewRuler-selectionHighlightForeground, var(--vscode-focusBorder)) max(var(--lane-selection-end), calc(var(--lane-selection-start) + 2px)),
        transparent max(var(--lane-selection-end), calc(var(--lane-selection-start) + 2px)));
    background-size: 3px 100%; background-position: right; background-repeat: no-repeat;
}
.timeline-scope::-webkit-scrollbar-thumb { background-color: var(--vscode-scrollbarSlider-background); border-radius: 0; }
.timeline-scope::-webkit-scrollbar-thumb:hover { background-color: var(--vscode-scrollbarSlider-hoverBackground); }
.timeline-scope::-webkit-scrollbar-thumb:active { background-color: var(--vscode-scrollbarSlider-activeBackground); }
.timeline-scope::-webkit-scrollbar-button { display: none; }
.timeline-scope::-webkit-scrollbar-corner { background: transparent; }
.timeline-scope.panning { cursor: grabbing; }
.time-region { min-width: 0; }
.timeline-heading { display: grid; grid-template-columns: var(--node-label-width) minmax(0, 1fr); }
.timeline-heading { box-sizing: border-box; position: sticky; top: 0; z-index: 1; background: var(--vscode-editor-background); flex: 0 0 var(--timeline-header-height); border-bottom: 1px solid var(--vscode-panel-border); }
.node-column { box-sizing: border-box; min-width: 0; padding: 8px 12px; border-right: 1px solid var(--vscode-panel-border); }
.lanes { flex: 0 0 auto; min-height: 0; position: relative; }
.playback-position { overflow: hidden; white-space: nowrap; font-variant-numeric: tabular-nums; }
.playback-position small { opacity: .6; margin-left: 6px; }
.lane { box-sizing: border-box; border-bottom: 1px solid var(--vscode-panel-border); }
.lane.selected { background: var(--vscode-list-inactiveSelectionBackground); outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
.lane:focus-visible { outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
.empty { padding: 12px; }
</style>

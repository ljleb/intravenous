<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef } from "vue";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { nodeDisplayName } from "../presentation/nodeSummary";
import TimeRuler from "../timeline/TimeRuler.vue";
import { provideTimelineNavigation, provideTimelineWindow } from "../timeline/timelineContext";
import { moveMiddleDrag, timelineMiddleDragAction, timelineWheelAction, wheelPixels, zoomTimelineHorizontally,
    zoomTimelineVertically, type MiddleDrag, type TimelineZoomAction } from "../../src/timelineViewport";
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
provideTimelineNavigation({ wheel, beginMiddleDrag });
const middleDrag = shallowRef<MiddleDrag | null>(null);
let middleDragMoved = false;
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
function zoomAnchor(action: TimelineZoomAction, event: MouseEvent): number {
    if (action === "horizontalZoom") {
        const axis = timeRegion.value?.getBoundingClientRect();
        return axis ? Math.max(0, Math.min(axis.width, event.clientX - axis.left)) : 0;
    }
    const contentTop = (root.value?.getBoundingClientRect().top ?? 0) + headerHeight;
    return Math.max(0, event.clientY - contentTop);
}
function wheel(event: WheelEvent) {
    const action = timelineWheelAction(event);
    if (action === "verticalScroll") return; // Native scrolling, including its bounds and momentum.
    event.preventDefault();
    event.stopPropagation();
    if (!root.value || !timeRegion.value) return;
    if (action === "horizontalScroll") {
        const axis = timeRegion.value.getBoundingClientRect();
        const horizontal = Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
        viewport.value = { ...viewport.value, beginSeconds: viewport.value.beginSeconds
            + wheelPixels(horizontal, event.deltaMode, axis.width) * viewport.value.secondsPerPixel };
    } else {
        const delta = wheelPixels(event.deltaY || event.deltaX, event.deltaMode, root.value.clientHeight);
        const anchor = zoomAnchor(action, event);
        viewport.value = action === "horizontalZoom" ? zoomTimelineHorizontally(viewport.value, anchor, delta)
            : zoomTimelineVertically(viewport.value, anchor, delta);
        if (action === "verticalZoom") void applyScroll();
    }
    scheduleSave();
}
function preventMiddleDefault(event: MouseEvent) {
    if (event.button === 1) event.preventDefault();
}
function beginMiddleDrag(event: PointerEvent) {
    const surface = root.value;
    if (event.button !== 1 || !surface || middleDrag.value) return;
    const targetTimeline = event.target instanceof Element ? event.target.closest(".timeline-scope") : null;
    if (targetTimeline && targetTimeline !== surface) return;
    event.preventDefault();
    event.stopPropagation();
    middleDrag.value = { pointerId: event.pointerId, x: event.clientX, y: event.clientY,
        action: timelineMiddleDragAction(event) };
    middleDragMoved = false;
    surface.setPointerCapture(event.pointerId);
}
function updateMiddleDrag(event: PointerEvent) {
    const drag = middleDrag.value;
    if (!drag || event.pointerId !== drag.pointerId) return;
    const view = viewport.value;
    const { action } = drag;
    const anchor = action !== "pan" && (action === "verticalZoom" ? event.clientX !== drag.x : event.clientY !== drag.y)
        ? zoomAnchor(action, event) : 0;
    const moved = moveMiddleDrag(view, drag, event.buttons, event.clientX, event.clientY, anchor);
    if (!moved) { endMiddleDrag(); return; }
    if (event.clientX === drag.x && event.clientY === drag.y) return;
    middleDrag.value = { ...drag, x: event.clientX, y: event.clientY };
    if (moved === view) return;
    middleDragMoved = true;
    viewport.value = moved;
    if (drag.action !== "horizontalZoom") void applyScroll();
    scheduleSave();
}
function endMiddleDrag(event?: PointerEvent) {
    const pointerId = middleDrag.value?.pointerId;
    if (pointerId === undefined || (event && event.pointerId !== pointerId)) return;
    middleDrag.value = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!middleDragMoved) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
const blurred = () => endMiddleDrag();
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
    if (middleDrag.value) endMiddleDrag();
    if (saveTimer) { clearTimeout(saveTimer); save(); }
});
</script>

<template>
    <section ref="root" class="timeline-scope"
        :data-middle-drag="middleDrag?.action" aria-label="Project timeline"
        :style="scrollbarStyle" :data-has-selection="selectedLaneIndex >= 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown.capture="beginMiddleDrag" @pointermove="updateMiddleDrag"
        @pointerup="endMiddleDrag" @pointercancel="endMiddleDrag" @lostpointercapture="endMiddleDrag"
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
.timeline-scope[data-middle-drag="pan"] { cursor: grabbing; }
.timeline-scope[data-middle-drag="horizontalZoom"] { cursor: ns-resize; }
.timeline-scope[data-middle-drag="verticalZoom"] { cursor: ew-resize; }
.time-region { min-width: 0; }
.timeline-heading { display: grid; grid-template-columns: var(--node-label-width) minmax(0, 1fr); }
.timeline-heading { box-sizing: border-box; position: sticky; top: 0; z-index: 1; background: var(--vscode-editor-background); flex: 0 0 var(--timeline-header-height); border-bottom: 1px solid var(--vscode-panel-border); }
.node-column { box-sizing: border-box; min-width: 0; padding: 8px 12px; border-right: 1px solid var(--vscode-panel-border); }
.lanes { flex: 0 0 auto; min-height: 0; position: relative; }
.playback-position { overflow: hidden; white-space: nowrap; font-variant-numeric: tabular-nums; color: var(--iv-lane-title-color); }
.playback-position small { color: var(--iv-port-label-base); margin-left: 6px; }
.lane { box-sizing: border-box; border-bottom: 1px solid var(--vscode-panel-border); }
.lane.selected { --iv-lane-title-color: var(--vscode-list-inactiveSelectionForeground, var(--vscode-foreground));
    background: var(--vscode-list-inactiveSelectionBackground); outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
.lane:focus-visible { outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
.empty { padding: 12px; }
</style>

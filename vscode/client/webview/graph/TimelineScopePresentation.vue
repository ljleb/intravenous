<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef } from "vue";
import { addressKey, type NodeAddress, type Point } from "../../src/graphScope";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { nodeDisplayName } from "../presentation/nodeSummary";
import TimeRuler from "../timeline/TimeRuler.vue";
import { provideTimelineNavigation, provideTimelineWindow, type LaneValueNavigation, type LaneValuePan } from "../timeline/timelineContext";
import { initialTimelineViewport, isMiddleDoubleClick, middleClickMode, moveMiddleDrag, panTimeline, resetTimelineZoom, timelineMiddleDragAction,
    timelineWheelAction, wheelPixels, wrapTimelineCursor, zoomAnchorHint, zoomMiddleDrag, zoomTimelineHorizontally, zoomTimelineVertically,
    type MiddleClick, type MiddleDrag, type TimelineZoomAction, type TimelineViewport } from "../../src/timelineViewport";
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
const { layout, save, copyNodeDebugDetails, selectedLane, selectLane, beginPointerLockExit, setTimelineZoomHint } = useGraphEditor();
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
const timeWindow = computed(() => {
    const { beginSeconds, secondsPerPixel } = viewport.value;
    const spanSeconds = Math.max(1, timeWidth.value) * secondsPerPixel;
    return { beginSeconds, endSeconds: beginSeconds + spanSeconds, spanSeconds, sampleRate: scope.value.sampleRate };
});
provideTimelineWindow(timeWindow);
const valuePanTargets = new Map<Element, LaneValueNavigation>();
provideTimelineNavigation({ wheel, beginMiddleDrag, registerValueNavigation(element, lane) {
    valuePanTargets.set(element, lane);
    return () => { valuePanTargets.delete(element); };
} });
const middleDrag = shallowRef<MiddleDrag | null>(null);
const wrappedCursor = shallowRef<Point | null>(null);
let laneValuePan: { lane: Element; pan: LaneValuePan } | null = null;
let lockedTravel: Point = { x: 0, y: 0 };
let middlePress: MiddleClick | null = null;
let lastMiddleClick: MiddleClick | null = null;
const middlePointerLock = new KnobPointerLock<Point>({
    request: () => root.value!.requestPointerLock(),
    exit: () => document.exitPointerLock(),
    locked: () => !!root.value && document.pointerLockElement === root.value,
}, moveLockedMiddleDrag, (id, _point, lockOrigin) => {
    if (lockOrigin) beginPointerLockExit(lockOrigin);
    finishMiddleDrag(id);
}, () => {
    // If the browser rejects the request after taking capture away, end the drag.
    const drag = middleDrag.value;
    if (drag && !root.value?.hasPointerCapture(drag.pointerId)) endMiddleDrag();
});
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
function zoomAnchor(action: TimelineZoomAction, x: number, y: number, unwrapped = false): number {
    if (action === "horizontalZoom") {
        const axis = timeRegion.value?.getBoundingClientRect();
        if (!axis) return 0;
        const relativeX = x - axis.left;
        return unwrapped ? relativeX : Math.max(0, Math.min(axis.width, relativeX));
    }
    const contentTop = (root.value?.getBoundingClientRect().top ?? 0) + headerHeight;
    return unwrapped ? y - contentTop : Math.max(0, y - contentTop);
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
        const anchor = zoomAnchor(action, event.clientX, event.clientY);
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
    const mode = middleClickMode(event);
    const lane = mode === "lane" && event.target instanceof Element ? event.target.closest(".lane") : null;
    middlePress = { x: event.clientX, y: event.clientY, at: performance.now(), mode, lane };
    const pan = lane ? valuePanTargets.get(lane)?.begin() : null;
    if (lane && pan) laneValuePan = { lane, pan };
    middleDragMoved = false;
    surface.setPointerCapture(event.pointerId);
    const { action } = middleDrag.value;
    if (action !== "pan") setTimelineZoomHint(zoomAnchorHint(action, { x: 0, y: 0 }));
    if (event.pointerType === "mouse" && !document.pointerLockElement)
        middlePointerLock.start(event.pointerId, { x: event.clientX, y: event.clientY });
}
function updateMiddleDrag(event: PointerEvent) {
    const drag = middleDrag.value;
    if (!drag || event.pointerId !== drag.pointerId || middlePointerLock.status().locked) return;
    if (event.clientX !== drag.x || event.clientY !== drag.y) cancelMiddleClick();
    const { action } = drag;
    const anchor = action === "pan" ? 0 : zoomAnchor(action, event.clientX, event.clientY);
    const moved = moveMiddleDrag(viewport.value, drag, event.buttons, event.clientX, event.clientY, anchor, laneValuePan !== null);
    if (!moved) { endMiddleDrag(); return; }
    if (event.clientX === drag.x && event.clientY === drag.y) return;
    middleDrag.value = { ...drag, x: event.clientX, y: event.clientY };
    applyMiddleDrag(moved);
    laneValuePan?.pan.move(event.clientY - drag.y);
}
function wrappedPosition(point: Point, travel: Point) {
    const surface = root.value!;
    const rect = surface.getBoundingClientRect();
    const left = Math.max(0, rect.left + surface.clientLeft);
    const top = Math.max(0, rect.top + surface.clientTop);
    const right = Math.min(window.innerWidth, rect.left + surface.clientLeft + surface.clientWidth);
    const bottom = Math.min(window.innerHeight, rect.top + surface.clientTop + surface.clientHeight);
    if (laneValuePan) {
        // The sticky ruler covers part of a lane that has scrolled past it.
        const lane = laneValuePan.lane.getBoundingClientRect();
        const laneLeft = Math.max(left, lane.left);
        const laneTop = Math.max(top, rect.top + surface.clientTop + headerHeight, lane.top);
        const laneRight = Math.min(right, lane.right);
        const laneBottom = Math.min(bottom, lane.bottom);
        if (laneRight > laneLeft && laneBottom > laneTop)
            return wrapTimelineCursor(point, travel, { left: laneLeft, top: laneTop, width: laneRight - laneLeft, height: laneBottom - laneTop });
    }
    return wrapTimelineCursor(point, travel, { left, top, width: right - left, height: bottom - top });
}
function moveLockedMiddleDrag(id: number, delta: Point) {
    const drag = middleDrag.value;
    if (!drag || drag.pointerId !== id || !root.value || !(delta.x || delta.y)) return;
    cancelMiddleClick();
    const { action } = drag;
    if (action === "pan") {
        wrappedCursor.value = wrappedPosition(wrappedCursor.value ?? drag, delta).visible;
        applyMiddleDrag(panTimeline(viewport.value, delta, laneValuePan !== null));
        laneValuePan?.pan.move(delta.y);
        return;
    }
    lockedTravel.x += delta.x;
    lockedTravel.y += delta.y;
    const { virtual, visible, screens } = wrappedPosition(drag, lockedTravel);
    wrappedCursor.value = visible;
    // Reconstruct the virtual position from the start and total travel. Only
    // the glyph wraps. Neither zoom anchor is calculated from the glyph.
    setTimelineZoomHint(zoomAnchorHint(action, screens));
    const anchor = zoomAnchor(action, virtual.x, virtual.y, true);
    applyMiddleDrag(zoomMiddleDrag(viewport.value, action, delta, anchor));
}
function applyMiddleDrag(moved: TimelineViewport) {
    const current = viewport.value;
    if (moved === current) return;
    middleDragMoved = true;
    viewport.value = moved;
    if (moved.scrollTop !== current.scrollTop || moved.laneHeight !== current.laneHeight) void applyScroll();
    scheduleSave();
}
function cancelMiddleClick() { middlePress = lastMiddleClick = null; }
function completeMiddleClick() {
    if (!middlePress) return;
    const click = middlePress;
    middlePress = null;
    if (performance.now() - click.at > 400) { lastMiddleClick = null; return; }
    if (!isMiddleDoubleClick(lastMiddleClick, click)) { lastMiddleClick = click; return; }
    lastMiddleClick = null;
    if (click.mode === "lane") {
        if (click.lane) valuePanTargets.get(click.lane)?.reset();
        return;
    }
    const axis = click.mode;
    applyMiddleDrag(axis === "view" ? { ...initialTimelineViewport }
        : resetTimelineZoom(viewport.value, axis, zoomAnchor(axis, click.x, click.y)));
}
function endMiddleDrag(event?: PointerEvent, clicked = event?.type === "pointerup") {
    const pointerId = middleDrag.value?.pointerId;
    if (pointerId === undefined || (event && event.pointerId !== pointerId)) return;
    if (event?.type === "pointerup" && event.button !== 1) return;
    if (clicked) completeMiddleClick();
    else cancelMiddleClick();
    middlePointerLock.cancel();
    finishMiddleDrag(pointerId);
}
function cancelCapturedMiddleDrag(event: PointerEvent) {
    if (!middlePointerLock.suppressPointerCancellation()) endMiddleDrag(event);
}
function finishMiddleDrag(pointerId: number) {
    const drag = middleDrag.value;
    if (drag?.pointerId !== pointerId) return;
    if (middlePress) cancelMiddleClick();
    middleDrag.value = null;
    wrappedCursor.value = null;
    if (drag.action !== "pan") setTimelineZoomHint(null);
    const laneChanged = laneValuePan?.pan.changed();
    laneValuePan = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!middleDragMoved && !laneChanged) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
function middleLockChanged() {
    middlePointerLock.changed();
    const drag = middleDrag.value;
    if (!drag || !root.value || !middlePointerLock.status().granted) return;
    lockedTravel = { x: 0, y: 0 };
    const { visible, screens } = wrappedPosition(drag, lockedTravel);
    wrappedCursor.value = visible;
    if (drag.action !== "pan") setTimelineZoomHint(zoomAnchorHint(drag.action, screens));
    if (root.value.hasPointerCapture(drag.pointerId)) root.value.releasePointerCapture(drag.pointerId);
}
function middleLockFailed() { middlePointerLock.error(); }
function lockedMiddleMove(event: MouseEvent) {
    if (middlePointerLock.move({ x: event.movementX, y: event.movementY })) event.stopImmediatePropagation();
}
function lockedMiddleRelease(event: MouseEvent) {
    if (event.button === 1 && middlePointerLock.status().locked) endMiddleDrag(undefined, true);
}
const blurred = () => { cancelMiddleClick(); endMiddleDrag(); };
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        for (const entry of entries) if (entry.target === timeRegion.value) timeWidth.value = entry.contentRect.width;
    });
    if (timeRegion.value) observer.observe(timeRegion.value);
    window.addEventListener("blur", blurred);
    window.addEventListener("mousemove", lockedMiddleMove, true);
    window.addEventListener("mouseup", lockedMiddleRelease, true);
    document.addEventListener("pointerlockchange", middleLockChanged);
    document.addEventListener("pointerlockerror", middleLockFailed);
    void applyScroll();
});
onBeforeUnmount(() => {
    disposed = true;
    observer?.disconnect();
    window.removeEventListener("blur", blurred);
    window.removeEventListener("mousemove", lockedMiddleMove, true);
    window.removeEventListener("mouseup", lockedMiddleRelease, true);
    document.removeEventListener("pointerlockchange", middleLockChanged);
    document.removeEventListener("pointerlockerror", middleLockFailed);
    if (middleDrag.value) endMiddleDrag();
    if (saveTimer) { clearTimeout(saveTimer); save(); }
});
</script>

<template>
    <section ref="root" class="timeline-scope"
        :data-middle-drag="middleDrag?.action" aria-label="Project timeline"
        :style="scrollbarStyle" :data-has-selection="selectedLaneIndex >= 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown.capture="beginMiddleDrag" @pointermove="updateMiddleDrag"
        @pointerup="endMiddleDrag" @pointercancel="cancelCapturedMiddleDrag" @lostpointercapture="cancelCapturedMiddleDrag"
        @mousedown="preventMiddleDefault" @auxclick="preventMiddleDefault">
        <header class="timeline-heading">
            <div class="node-column playback-position" data-status-hint="Space ▶/Ⅱ">
                <template v-if="playbackSeconds !== null">
                    {{ playback.state.value?.running ? '▷' : 'Ⅱ' }} {{ playbackSeconds.toFixed(3) }} s
                    <small>{{ playback.position.value?.toString() }} smp</small>
                </template>
            </div>
            <div ref="timeRegion" class="time-region"><TimeRuler :window="timeWindow" :sample-rate="scope.sampleRate ?? undefined" playback style="height: 100%" /></div>
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
    <Teleport to="body">
        <svg v-if="wrappedCursor" class="wrapped-cursor" :style="{ left: `${wrappedCursor.x}px`, top: `${wrappedCursor.y}px` }"
            viewBox="0 0 20 20" aria-hidden="true">
            <path :transform="middleDrag?.action === 'verticalZoom' ? 'rotate(90 10 10)' : undefined"
                d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
            <path v-if="middleDrag?.action === 'pan'" transform="rotate(90 10 10)" d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
        </svg>
    </Teleport>
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
.wrapped-cursor {
    position: fixed; z-index: 1000; width: 20px; height: 20px; transform: translate(-50%, -50%);
    pointer-events: none; fill: none; stroke: var(--vscode-editor-foreground); stroke-width: 1.5;
    stroke-linecap: round; stroke-linejoin: round; filter: drop-shadow(0 0 1px var(--vscode-editor-background));
}
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

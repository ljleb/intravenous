<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef } from "vue";
import { addressKey, type NodeAddress, type Point } from "../../src/graphScope";
import { orderLanes, selectedLaneRuns } from "../../src/laneSelection";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { listenPortConnectionFocus } from "../../src/portConnectionFocus";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { nodeDisplayName } from "../presentation/nodeSummary";
import TimeRuler from "../timeline/TimeRuler.vue";
import { provideTimelineNavigation, provideTimelineWindow, type LaneValueGesture, type LaneValueNavigation } from "../timeline/timelineContext";
import { initialTimelineViewport, isMiddleDoubleClick, middleClickMode, moveMiddleDrag, panTimeline, resetTimelineZoom, timelineMiddleDragAction,
    timelineWheelAction, verticalMiddleDragZoomFactor, wheelPixels, wrapTimelineCursor, zoomAnchorHint, zoomFactorForPixels, zoomMiddleDrag,
    zoomTimelineHorizontally, zoomTimelineVertically,
    type MiddleClick, type MiddleDrag, type TimelineZoomAction, type TimelineViewport } from "../../src/timelineViewport";
import { useGraphEditor, type CursorWarpAnchor } from "./context";
import { usePlayback } from "../timeline/playbackContext";

const node = useNode();
const playback = usePlayback();
const backgroundPlayheadSeconds = computed(() => playback.backgroundPlayhead.value === null ? null
    : Number(playback.backgroundPlayhead.value) / (playback.state.value?.sampleRate ?? 48000));
const scope = computed(() => node.value.scope!);
const { layout, save, copyNodeDebugDetails, selectedLanes, selectLane, beginPointerLockExit, takeMiddleCursorTrace,
    reportCursorReturnSkipped, setTimelineZoomHint } = useGraphEditor();
const children = computed(() => orderLanes(scope.value.nodes, layout.value.laneOrder).map((child) => ({
    child, semantic: semanticNodeFromScopeChild(child, scope.value),
})));
function activateLane(event: PointerEvent, address: NodeAddress) {
    if (event.button === 0 || event.button === 2) selectLane(address, event);
}
const viewport = computed({ get: () => layout.value.timeline,
    set: (value) => { layout.value.timeline = value; } });
const laneHeight = computed(() => viewport.value.laneHeight);
const headerHeight = 36;
const scrollbarStyle = computed(() => {
    const contentHeight = headerHeight + scope.value.nodes.length * laneHeight.value;
    const accent = "var(--vscode-editorOverviewRuler-selectionHighlightForeground, var(--vscode-focusBorder))";
    const marks = selectedLaneRuns(children.value.map(({ child }) => addressKey(child.address)), selectedLanes.value).flatMap((run) => {
        const start = 100 * (headerHeight + run.begin * laneHeight.value) / contentHeight;
        const end = 100 * (headerHeight + run.end * laneHeight.value) / contentHeight;
        return [`transparent ${start}%`, `${accent} ${start}% ${end}%`, `transparent ${end}%`];
    });
    return { "--timeline-header-height": `${headerHeight}px`,
        "--lane-selection-markers": marks.length ? `linear-gradient(to bottom, ${marks.join(", ")})` : "none" };
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
const valueNavigationTargets = new Map<Element, LaneValueNavigation>();
provideTimelineNavigation({ wheel, beginMiddleDrag, registerValueNavigation(element, lane) {
    valueNavigationTargets.set(element, lane);
    return () => { valueNavigationTargets.delete(element); };
} });
const middleDrag = shallowRef<MiddleDrag | null>(null);
const wrappedCursor = shallowRef<(Point & { action: MiddleDrag["action"] | "valueZoom" }) | null>(null);
let laneValueGesture: { lane: Element; gesture: LaneValueGesture } | null = null;
let lockedTravel: Point = { x: 0, y: 0 };
let middlePress: MiddleClick | null = null;
let lastMiddleClick: MiddleClick | null = null;
let middleCursorAnchor: (CursorWarpAnchor & { moved: boolean }) | null = null;
let middlePointerLockError = "";
let middleCursorTrace: ReturnType<typeof takeMiddleCursorTrace> = null;
let tracedLockedMoves = 0;
const middlePointerLock = new KnobPointerLock<Point>({
    request: () => {
        if (document.pointerLockElement && document.pointerLockElement !== root.value)
            throw new Error("Another gesture owns pointer lock");
        return root.value!.requestPointerLock();
    },
    exit: () => document.exitPointerLock(),
    locked: () => !!root.value && document.pointerLockElement === root.value,
}, moveLockedMiddleDrag, (id, release, lockOrigin) => {
    const destination = wrappedCursor.value;
    const warp = release && lockOrigin && destination && middleCursorAnchor?.moved
        ? { ...middleCursorAnchor, destination, trace: !!middleCursorTrace } : undefined;
    middleCursorTrace?.(`Drag finished. ${JSON.stringify({ release, lockOrigin, destination, lockedMoves: tracedLockedMoves, queued: !!warp })}`);
    if (lockOrigin) beginPointerLockExit(lockOrigin, warp);
    if (middleCursorAnchor?.moved && (!release || !lockOrigin || !destination)) {
        reportCursorReturnSkipped(release && !lockOrigin
            ? middlePointerLockError || "The middle-button drag used pointer capture."
            : "The middle-button drag ended before a normal pointer-lock release.");
    }
    middleCursorTrace = null;
    middleCursorAnchor = null;
    finishMiddleDrag(id);
}, (message) => {
    middlePointerLockError = message;
    middleCursorTrace?.(`Pointer lock request failed. ${message}`);
    // If the browser rejects the request after taking capture away, end the drag.
    const drag = middleDrag.value;
    if (drag && !root.value?.hasPointerCapture(drag.pointerId)) endMiddleDrag("Pointer capture was unavailable after lock failed.");
});
let middleDragMoved = false;
let observer: ResizeObserver | null = null;
let saveTimer: ReturnType<typeof setTimeout> | undefined;
let scrollRequest = 0;
let disposed = false;
let stopListeningForFocus: (() => void) | undefined;

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
    const lane = event.shiftKey && event.target instanceof Element ? event.target.closest(".lane") : null;
    const navigation = lane && valueNavigationTargets.get(lane);
    const action = timelineWheelAction(event, !!navigation);
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
        if (action === "laneValueZoom") {
            if (navigation?.zoom(zoomFactorForPixels(-delta), event.clientY)) scheduleSave();
            return;
        }
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
function preventMiddleDragDefault(event: MouseEvent) {
    if (middleDrag.value) preventMiddleDefault(event);
}
function beginMiddleDrag(event: PointerEvent) {
    const surface = root.value;
    if (event.button !== 1 || !surface || middleDrag.value) return;
    const targetTimeline = event.target instanceof Element ? event.target.closest(".timeline-scope") : null;
    if (targetTimeline && targetTimeline !== surface) return;
    const trace = takeMiddleCursorTrace();
    trace?.(`Press. ${JSON.stringify({ pointerId: event.pointerId, pointerType: event.pointerType, buttons: event.buttons,
        clientX: event.clientX, clientY: event.clientY, screenX: event.screenX, screenY: event.screenY, scale: window.devicePixelRatio,
        active: window.navigator?.userActivation?.isActive, shift: event.shiftKey, alt: event.altKey, ctrl: event.ctrlKey })}`);
    if (document.pointerLockElement && document.pointerLockElement !== surface) {
        trace?.("Another gesture owns pointer lock.");
        return;
    }
    try {
        surface.setPointerCapture(event.pointerId);
    } catch (error) {
        if (error instanceof DOMException && error.name === "NotFoundError") {
            trace?.("The browser refused pointer capture because the pointer was no longer active.");
            return;
        }
        throw error;
    }
    event.preventDefault();
    event.stopPropagation();
    const lane = event.shiftKey && event.target instanceof Element ? event.target.closest(".lane") : null;
    const navigation = lane && valueNavigationTargets.get(lane);
    const mode = middleClickMode(event, !!navigation);
    middlePress = { x: event.clientX, y: event.clientY, at: performance.now(), mode, lane: mode === "lane" ? lane : null };
    const laneMode = event.altKey ? "zoom" : "pan";
    const gesture = mode === "lane" && navigation ? navigation.begin(laneMode) : null;
    if (lane && gesture) laneValueGesture = { lane, gesture };
    const action = laneValueGesture ? "pan" : timelineMiddleDragAction(event);
    middleDrag.value = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, action };
    middleDragMoved = false;
    middlePointerLockError = "";
    middleCursorTrace = trace;
    tracedLockedMoves = 0;
    middleCursorAnchor = event.pointerType === "mouse" ? {
        screenOrigin: { x: event.screenX, y: event.screenY }, scale: window.devicePixelRatio, moved: false,
    } : null;
    if (laneValueGesture?.gesture.mode === "zoom") setTimelineZoomHint(zoomAnchorHint("valueZoom", { x: 0, y: 0 }));
    else if (action !== "pan") setTimelineZoomHint(zoomAnchorHint(action, { x: 0, y: 0 }));
    middleCursorTrace?.(`Started ${laneValueGesture ? `lane ${laneMode}` : action}. Pointer capture is ready.`);
    middlePointerLock.start(event.pointerId, { x: event.clientX, y: event.clientY }, event.pointerType === "mouse");
}
function updateMiddleDrag(event: PointerEvent) {
    const drag = middleDrag.value;
    if (!drag || event.pointerId !== drag.pointerId || middlePointerLock.status().locked) return;
    if (event.clientX !== drag.x || event.clientY !== drag.y) cancelMiddleClick();
    const { action } = drag;
    const anchor = action === "pan" ? 0 : zoomAnchor(action, event.clientX, event.clientY);
    const moved = moveMiddleDrag(viewport.value, drag, event.buttons, event.clientX, event.clientY, anchor, laneValueGesture !== null);
    if (!moved) { endMiddleDrag("A captured move reported the middle button up."); return; }
    if (event.clientX === drag.x && event.clientY === drag.y) return;
    if (middleCursorAnchor && !middleCursorAnchor.moved) {
        middleCursorTrace?.("The first move used pointer capture.");
        middleCursorAnchor.moved = true;
    }
    middleDrag.value = { ...drag, x: event.clientX, y: event.clientY };
    if (laneValueGesture?.gesture.mode !== "zoom") applyMiddleDrag(moved);
    moveLaneValueDrag({ x: event.clientX - drag.x, y: event.clientY - drag.y }, event.clientY);
}
function moveLaneValueDrag(delta: Point, cursorY: number) {
    const gesture = laneValueGesture?.gesture;
    if (gesture?.mode === "zoom" && delta.x) gesture.zoom(verticalMiddleDragZoomFactor(delta.x), cursorY);
    if (gesture?.mode === "pan" && delta.y) gesture.pan(delta.y);
}
function wrappedPosition(point: Point, travel: Point) {
    const surface = root.value!;
    const rect = surface.getBoundingClientRect();
    const left = Math.max(0, rect.left + surface.clientLeft);
    const top = Math.max(0, rect.top + surface.clientTop);
    const right = Math.min(window.innerWidth, rect.left + surface.clientLeft + surface.clientWidth);
    const bottom = Math.min(window.innerHeight, rect.top + surface.clientTop + surface.clientHeight);
    if (laneValueGesture) {
        // The sticky ruler covers part of a lane that has scrolled past it.
        const { lane: element, gesture } = laneValueGesture;
        const { mode } = gesture;
        const lane = (mode === "pan" ? gesture.content : element).getBoundingClientRect();
        const laneLeft = Math.max(left, lane.left);
        const laneTop = Math.max(top, rect.top + surface.clientTop + headerHeight, lane.top);
        const laneRight = Math.min(right, lane.right);
        const laneBottom = Math.min(bottom, lane.bottom);
        if (mode === "pan" || (laneRight > laneLeft && laneBottom > laneTop))
            return wrapTimelineCursor(point, travel, { left: laneLeft, top: laneTop, width: laneRight - laneLeft, height: laneBottom - laneTop });
    }
    return wrapTimelineCursor(point, travel, { left, top, width: right - left, height: bottom - top });
}
function moveLockedMiddleDrag(id: number, delta: Point) {
    const drag = middleDrag.value;
    if (!drag || drag.pointerId !== id || !root.value || !(delta.x || delta.y)) return;
    if (middleCursorAnchor) middleCursorAnchor.moved = true;
    if (middleCursorTrace && ++tracedLockedMoves === 1) middleCursorTrace(`First locked move. ${JSON.stringify(delta)}`);
    cancelMiddleClick();
    const { action } = drag;
    if (laneValueGesture?.gesture.mode === "zoom") {
        lockedTravel.x += delta.x;
        lockedTravel.y += delta.y;
        const { visible, virtual, screens } = wrappedPosition(drag, lockedTravel);
        wrappedCursor.value = { ...visible, action: "valueZoom" };
        setTimelineZoomHint(zoomAnchorHint("valueZoom", screens));
        moveLaneValueDrag(delta, virtual.y);
        return;
    }
    if (action === "pan") {
        const { visible } = wrappedPosition(wrappedCursor.value ?? drag, delta);
        wrappedCursor.value = { ...visible, action };
        applyMiddleDrag(panTimeline(viewport.value, delta, laneValueGesture !== null));
        moveLaneValueDrag(delta, visible.y);
        return;
    }
    lockedTravel.x += delta.x;
    lockedTravel.y += delta.y;
    const { virtual, visible, screens } = wrappedPosition(drag, lockedTravel);
    wrappedCursor.value = { ...visible, action };
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
        if (click.lane) valueNavigationTargets.get(click.lane)?.reset();
        return;
    }
    const axis = click.mode;
    applyMiddleDrag(axis === "view" ? { ...initialTimelineViewport }
        : resetTimelineZoom(viewport.value, axis, zoomAnchor(axis, click.x, click.y)));
}
function endMiddleDrag(reason: string, event?: PointerEvent) {
    const pointerId = middleDrag.value?.pointerId;
    if (pointerId === undefined || (event && event.pointerId !== pointerId)) return;
    middleCursorTrace?.(reason);
    cancelMiddleClick();
    middlePointerLock.cancel();
}
function middleCaptureLost(event: PointerEvent) {
    if (event.target === root.value && !middlePointerLock.suppressPointerCancellation()) endMiddleDrag("The drag lost pointer capture.", event);
}
function middlePointerCancelled(event: PointerEvent) {
    if (event.target === root.value) endMiddleDrag("The browser cancelled the pointer.", event);
}
function finishMiddleDrag(pointerId: number) {
    const drag = middleDrag.value;
    if (drag?.pointerId !== pointerId) return;
    if (middlePress) cancelMiddleClick();
    middleDrag.value = null;
    // Keep the wrapped cursor visible until the browser restores its cursor.
    if (document.pointerLockElement !== root.value) wrappedCursor.value = null;
    if (drag.action !== "pan" || laneValueGesture?.gesture.mode === "zoom") setTimelineZoomHint(null);
    const laneChanged = laneValueGesture?.gesture.changed();
    laneValueGesture = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!middleDragMoved && !laneChanged) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
function middleLockChanged() {
    middleCursorTrace?.(document.pointerLockElement === root.value ? "The timeline acquired pointer lock." : "The timeline lost pointer lock.");
    middlePointerLock.changed();
    const drag = middleDrag.value;
    if (!drag && document.pointerLockElement !== root.value) wrappedCursor.value = null;
    if (!drag || !root.value || !middlePointerLock.status().granted) return;
    lockedTravel = { x: 0, y: 0 };
    const { visible, screens } = wrappedPosition(drag, lockedTravel);
    wrappedCursor.value = { ...visible, action: laneValueGesture?.gesture.mode === "zoom" ? "valueZoom" : drag.action };
    if (laneValueGesture?.gesture.mode === "zoom") setTimelineZoomHint(zoomAnchorHint("valueZoom", screens));
    else if (drag.action !== "pan") setTimelineZoomHint(zoomAnchorHint(drag.action, screens));
    if (root.value.hasPointerCapture(drag.pointerId)) root.value.releasePointerCapture(drag.pointerId);
}
function middleLockFailed() { middlePointerLock.error(); }
function lockedMiddleMove(event: MouseEvent) {
    const drag = middleDrag.value;
    if (!drag || document.pointerLockElement !== root.value) return;
    const delta = { x: event.movementX, y: event.movementY };
    if (!delta.x && !delta.y) return;
    if (!(event.buttons & 4) && middlePointerLock.status().granted) {
        // A real move with the button up can stand in for a missing mouseup.
        // Finish through release so the cursor keeps its last wrapped point.
        middleCursorTrace?.("A locked move reported the middle button up. Releasing the last wrapped position.");
        cancelMiddleClick();
        middlePointerLock.release(drag.pointerId, { x: event.clientX, y: event.clientY });
        event.stopImmediatePropagation();
    } else if (middlePointerLock.move(delta)) event.stopImmediatePropagation();
}
function releaseMiddleDrag(event: MouseEvent) {
    const drag = middleDrag.value;
    if (!drag || event.button !== 1) return;
    if ("pointerId" in event && event.pointerId !== drag.pointerId) {
        middleCursorTrace?.(`Ignored ${event.type} for pointer ${event.pointerId}. Expected ${drag.pointerId}.`);
        return;
    }
    middleCursorTrace?.(`Release event ${event.type}. ${JSON.stringify({ clientX: event.clientX, clientY: event.clientY, buttons: event.buttons })}`);
    try {
        completeMiddleClick();
    } finally {
        middlePointerLock.release(drag.pointerId, { x: event.clientX, y: event.clientY });
    }
}
function nativeFocusChanged(focused: boolean) {
    if (!focused) {
        cancelMiddleClick();
        endMiddleDrag("The editor window lost native focus.");
    }
}
function middleVisibilityChanged() {
    if (document.hidden) {
        cancelMiddleClick();
        endMiddleDrag("The graph webview was hidden.");
    }
}
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        for (const entry of entries) if (entry.target === timeRegion.value) timeWidth.value = entry.contentRect.width;
    });
    if (timeRegion.value) observer.observe(timeRegion.value);
    // Chromium can blur the webview while activating its native window.
    // Keep the drag through that blur and end it on native focus loss.
    stopListeningForFocus = listenPortConnectionFocus(window, () => {
        if (!middleDrag.value) cancelMiddleClick();
    }, nativeFocusChanged);
    window.addEventListener("mousedown", preventMiddleDragDefault, true);
    window.addEventListener("mousemove", lockedMiddleMove, true);
    window.addEventListener("pointerup", releaseMiddleDrag, true);
    window.addEventListener("mouseup", releaseMiddleDrag, true);
    document.addEventListener("pointerlockchange", middleLockChanged);
    document.addEventListener("pointerlockerror", middleLockFailed);
    document.addEventListener("visibilitychange", middleVisibilityChanged);
    void applyScroll();
});
onBeforeUnmount(() => {
    disposed = true;
    observer?.disconnect();
    stopListeningForFocus?.();
    window.removeEventListener("mousedown", preventMiddleDragDefault, true);
    window.removeEventListener("mousemove", lockedMiddleMove, true);
    window.removeEventListener("pointerup", releaseMiddleDrag, true);
    window.removeEventListener("mouseup", releaseMiddleDrag, true);
    document.removeEventListener("pointerlockchange", middleLockChanged);
    document.removeEventListener("pointerlockerror", middleLockFailed);
    document.removeEventListener("visibilitychange", middleVisibilityChanged);
    if (middleDrag.value) endMiddleDrag("The timeline was removed during the drag.");
    if (saveTimer) { clearTimeout(saveTimer); save(); }
});
</script>

<template>
    <section ref="root" class="timeline-scope"
        :data-middle-drag="middleDrag && (laneValueGesture?.gesture.mode === 'zoom' ? 'valueZoom' : middleDrag.action)" aria-label="Project timeline"
        :style="scrollbarStyle" :data-has-selection="selectedLanes.size > 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown.capture="beginMiddleDrag" @pointermove="updateMiddleDrag"
        @pointercancel="middlePointerCancelled" @lostpointercapture="middleCaptureLost"
        @mousedown.capture="preventMiddleDefault" @auxclick.capture="preventMiddleDefault">
        <header class="timeline-heading">
            <div class="node-column playback-position" data-status-shortcuts="Space ▶/Ⅱ">
                <template v-if="backgroundPlayheadSeconds !== null">
                    {{ playback.state.value?.running ? '▷' : 'Ⅱ' }} {{ backgroundPlayheadSeconds.toFixed(3) }} s
                    <small>{{ playback.backgroundPlayhead.value?.toString() }} smp</small>
                </template>
            </div>
            <div ref="timeRegion" class="time-region"><TimeRuler :window="timeWindow" :sample-rate="scope.sampleRate ?? undefined" playback style="height: 100%" /></div>
        </header>
        <div class="lanes">
            <section v-for="{ child, semantic } in children" :key="addressKey(child.address)"
                class="lane" :class="{ selected: selectedLanes.has(addressKey(child.address)) }"
                :style="{ height: `${laneHeight}px` }" tabindex="0"
                :aria-label="nodeDisplayName(semantic) + (selectedLanes.has(addressKey(child.address)) ? ', selected' : '')"
                @pointerdown.capture="activateLane($event, child.address)" @keydown.enter.self="selectLane(child.address, $event)">
                <PresentationHost :node="semantic" :region-height="laneHeight - 1" lane
                    @copy-debug-details="copyNodeDebugDetails(child.address)" />
            </section>
            <div v-if="!children.length" class="empty">No project instances.</div>
        </div>
    </section>
    <Teleport to="body">
        <svg v-if="wrappedCursor" class="wrapped-cursor" :style="{ left: `${wrappedCursor.x}px`, top: `${wrappedCursor.y}px` }"
            viewBox="0 0 20 20" aria-hidden="true">
            <path :transform="wrappedCursor.action === 'verticalZoom' || wrappedCursor.action === 'valueZoom' ? 'rotate(90 10 10)' : undefined"
                d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
            <path v-if="wrappedCursor.action === 'pan'" transform="rotate(90 10 10)" d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
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
    background-image: var(--lane-selection-markers);
    background-size: 3px 100%; background-position: right; background-repeat: no-repeat;
}
.timeline-scope::-webkit-scrollbar-thumb { background-color: var(--vscode-scrollbarSlider-background); border-radius: 0; }
.timeline-scope::-webkit-scrollbar-thumb:hover { background-color: var(--vscode-scrollbarSlider-hoverBackground); }
.timeline-scope::-webkit-scrollbar-thumb:active { background-color: var(--vscode-scrollbarSlider-activeBackground); }
.timeline-scope::-webkit-scrollbar-button { display: none; }
.timeline-scope::-webkit-scrollbar-corner { background: transparent; }
.timeline-scope[data-middle-drag="pan"] { cursor: grabbing; }
.timeline-scope[data-middle-drag="horizontalZoom"] { cursor: ns-resize; }
.timeline-scope:is([data-middle-drag="verticalZoom"], [data-middle-drag="valueZoom"]) { cursor: ew-resize; }
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
.lane { position: relative; box-sizing: border-box; border-bottom: 1px solid var(--vscode-panel-border); }
.lane:focus-visible { outline: none; }
.lane.selected::after, .lane:focus-visible::after {
    content: ""; position: absolute; z-index: 0; pointer-events: none;
    inset-inline-start: calc(var(--node-label-width) - 1px); inset-block: 0; width: 2px;
    background: var(--vscode-tab-activeBorderTop, var(--vscode-tab-activeBorder,
        var(--vscode-panelTitle-activeBorder, var(--vscode-focusBorder))));
}
.lane:focus-visible:not(.selected)::after { background: var(--vscode-focusBorder); }
.lane.selected:has(+ .lane.selected)::after { inset-block-end: -1px; }
.empty { padding: 12px; }
</style>

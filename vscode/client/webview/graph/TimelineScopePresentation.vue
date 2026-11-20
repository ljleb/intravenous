<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch, onScopeDispose } from "vue";
import { addressKey, type Point } from "../../src/graphScope";
import { isTimelineSnapAddress, isTimelineSnapNode, managedBeats, managedTimeline, timelineGridMode, type TimelineGridMode } from "../../src/timelineSnap";
import { orderLanes, laneRuns } from "../../src/laneSelection";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { listenPortConnectionFocus } from "../../src/portConnectionFocus";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { presentations } from "../presentation/presentations";
import { nodeDisplayName } from "../presentation/nodeSummary";
import LaneSettings from "../presentation/LaneSettings.vue";
import BeatsSettings from "../presentation/BeatsSettings.vue";
import NodePorts from "../presentation/NodePorts.vue";
import { useBeatGridEditing } from "../presentation/beatGridEditingContext";
import TimeRuler from "../timeline/TimeRuler.vue";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import type { SeekBeatReader } from "../timeline/usePlayheadSeek";
import { timelineBeatObservationWindow } from "../timeline/geometry";
import { minimumBeatMarkSpacing, readBeatGrid, selectVisibleBeatMarks } from "../../src/beatGrid";
import { gridGuideStrengths } from "../../src/gridGuideStrength";
import { provideTimelineMarks, provideTimelineNavigation, provideTimelineWindow, type TimelineMark,
    type LaneHorizontalZoomGesture, type LaneValueGesture, type LaneValueNavigation } from "../timeline/timelineContext";
import { initialTimelineViewport, isMiddleDoubleClick, middleClickMode, middleDragZoomFactor, middleZoomPickup, moveMiddleDrag,
    panTimeline, resetTimelineZoom, timelineMiddleDragAction, timelineWheelAction, wheelPixels, wrapTimelineCursor,
    zoomAnchorHint, zoomFactorForPixels, zoomMiddleDrag, zoomMiddleDragAtPickup, zoomTimelineAxes,
    type MiddleClick, type MiddleDrag, type MiddleZoomPickup, type TimelineZoomAction, type TimelineViewport } from "../../src/timelineViewport";
import { useGraphEditor } from "./context";
import { cursorWarpAnchor, type CursorWarpAnchor } from "./cursorReturn";
import { usePlayback } from "../timeline/playbackContext";
import { useObservations } from "../presentation/observationContext";
import { sameObservationWindow, type ObservationWindow, type ObservationWindowResult } from "../../src/observations";
import { automationSampleIndex, automationSampleOffset } from "../../src/automationCurveGeometry";
import { webviewApi } from "../host";

const node = useNode();
const playback = usePlayback();
const backgroundPlayheadSeconds = computed(() => playback.backgroundPlayhead.value === null ? null
    : Number(playback.backgroundPlayhead.value) / (playback.state.value?.sampleRate ?? 48000));
const scope = computed(() => node.value.scope!);
const { layout, save, copyNodeDebugDetails, selectedLanes, finishPointerLock, setTimelineZoomHint } = useGraphEditor();
function renameLane(instanceId: string, displayName: string, previousDisplayName: string) {
    webviewApi.postMessage({ type: "renameGraphLane", instanceId, displayName, previousDisplayName });
}
const children = computed(() => orderLanes(scope.value.nodes.filter((node) => !isTimelineSnapNode(node)), layout.value.laneOrder).map((child) => ({
    child, semantic: semanticNodeFromScopeChild(child, scope.value),
})));
const laneOrder = computed(() => children.value.map(({ child }) => addressKey(child.address)));
const viewport = computed({ get: () => layout.value.timeline,
    set: (value) => { layout.value.timeline = value; } });
const gridMode = computed(() => timelineGridMode(scope.value));
const gridModes = [
    { mode: "seconds", label: "Seconds", description: "Time graduations" },
    { mode: "beats", label: "Beats", description: "Internal beat grid" },
    { mode: "custom", label: "Custom", description: "Grid from connected Beat Events" },
] as const;
const gridLabel = computed(() => gridModes.find(({ mode }) => mode === gridMode.value)!.label);
const beatsNode = computed(() => managedBeats(scope.value));
const settingsGrid = computed(() => beatsNode.value?.loadState === "ready" && beatsNode.value.construction?.definitionId === "builtin.beats"
    ? readBeatGrid(beatsNode.value.construction.arguments) : null);
const snapTimeline = computed(() => managedTimeline(scope.value));
const customInput = computed(() => gridMode.value === "custom" && snapTimeline.value
    ? semanticNodeFromScopeChild(snapTimeline.value, scope.value) : null);
const { edits: beatEdits } = useBeatGridEditing();
async function setGridMode(mode: TimelineGridMode) {
    if (!scope.value.editable || gridMode.value === mode) return;
    if (beatsNode.value) await beatEdits.finish(beatsNode.value.address);
    webviewApi.postMessage({ type: "setTimelineSnapMode", mode });
}
const laneHeight = computed(() => viewport.value.laneHeight);
const timelineRuns = computed(() => {
    const timelineLanes = new Set(children.value.filter(({ semantic }) => presentations.select(semantic.definitionId).timeline)
        .map(({ child }) => addressKey(child.address)));
    const height = laneHeight.value;
    return laneRuns(laneOrder.value, timelineLanes).map(({ begin, end }) => ({
        begin, style: { top: `${begin * height}px`, height: `${(end - begin) * height}px` },
    }));
});
const headerHeight = computed(() => gridMode.value === "custom" ? 60 : 36);
const scrollbarStyle = computed(() => {
    const height = headerHeight.value;
    const contentHeight = height + children.value.length * laneHeight.value;
    const accent = "var(--vscode-editorOverviewRuler-selectionHighlightForeground, var(--vscode-focusBorder))";
    const marks = laneRuns(laneOrder.value, selectedLanes.value).flatMap((run) => {
        const start = 100 * (height + run.begin * laneHeight.value) / contentHeight;
        const end = 100 * (height + run.end * laneHeight.value) / contentHeight;
        return [`transparent ${start}%`, `${accent} ${start}% ${end}%`, `transparent ${end}%`];
    });
    return { "--timeline-header-height": `${height}px`,
        "--lane-selection-markers": marks.length ? `linear-gradient(to bottom, ${marks.join(", ")})` : "none" };
});
const root = ref<HTMLElement | null>(null);
const timeRegion = ref<HTMLElement | null>(null);
const timeWidth = ref(0);
const timeWindow = computed(() => {
    const { beginSeconds, secondsPerPixel } = viewport.value;
    const spanSeconds = Math.max(1, timeWidth.value) * secondsPerPixel;
    return { beginSeconds, endSeconds: beginSeconds + spanSeconds, spanSeconds, sampleRate: scope.value.sampleRate,
        gridMode: gridMode.value };
});
provideTimelineWindow(timeWindow);
const observations = useObservations();
const timelineInterest = shallowRef<string>();
const observedTimeline = computed(() => scope.value.timelineNodes?.find(isTimelineSnapAddress));
const decodedBeatMarks = new WeakMap<NonNullable<ObservationWindowResult["marks"]>, Array<Pick<TimelineMark, "index" | "depth">>>();
const beatViewport = computed(() => {
    const { beginSeconds, spanSeconds, sampleRate } = timeWindow.value;
    if (gridMode.value === "seconds" || !sampleRate || timeWidth.value <= 0) return undefined;
    const first = automationSampleIndex(beginSeconds, sampleRate);
    const last = automationSampleIndex(beginSeconds, sampleRate, spanSeconds);
    return first === undefined || last === undefined || last < first ? undefined : { first, length: last - first + 1n };
});
const observedTimelineWindow = computed<ObservationWindow | undefined>((previous) => {
    const visible = beatViewport.value;
    if (!visible) return;
    const window = timelineBeatObservationWindow(visible.first, visible.length, timeWidth.value);
    return sameObservationWindow(window, previous) ? previous : window;
});
watch([gridMode, observedTimeline, observedTimelineWindow], ([mode, node, window], previous) => {
    if (!observations) return;
    if (timelineInterest.value && (mode !== previous[0] || !node || !window
        || !previous[1] || addressKey(node) !== addressKey(previous[1]))) {
        observations.observePort(timelineInterest.value, undefined, "viewport");
        timelineInterest.value = undefined;
    }
    if (!node || !window) return;
    const id = timelineInterest.value ??= observations.portObservationId();
    observations.observePort(id, {
        source: { node, kind: "input", member: "beats", portKind: "event" }, operation: "beat-grid", window,
    }, "viewport");
}, { immediate: true });
onScopeDispose(() => {
    if (timelineInterest.value) observations?.observePort(timelineInterest.value, undefined);
});
function projectBeatMarks(raw: NonNullable<ObservationWindowResult["marks"]>, first: bigint,
    pixelsPerSample: number, width: number, originX = 0) {
    let marks = decodedBeatMarks.get(raw);
    if (!marks) {
        marks = raw.map(({ index, depth }) => ({ index: BigInt(index), depth }));
        decodedBeatMarks.set(raw, marks);
    }
    const candidates = marks.map(({ index, depth }) => ({ index, depth,
        x: originX + Number(BigInt.asIntN(64, index - first)) * pixelsPerSample }))
        .filter(({ x }) => x >= -minimumBeatMarkSpacing && x <= width + minimumBeatMarkSpacing);
    return selectVisibleBeatMarks(candidates, width);
}
const timelineMarks = computed<readonly TimelineMark[]>((previous) => {
    const interest = timelineInterest.value;
    if (gridMode.value === "seconds" || !observations || !interest) return [];
    const { beginSeconds, spanSeconds, sampleRate } = timeWindow.value;
    const visible = beatViewport.value, window = observedTimelineWindow.value;
    const width = timeWidth.value;
    if (!sampleRate || !visible || !window) return [];
    const pixelsPerSample = width / (spanSeconds * sampleRate);
    const originX = automationSampleOffset(visible.first, beginSeconds, sampleRate) / spanSeconds * width;
    const result = observations.windowForDisplay(interest, window);
    if (result?.status !== "ready" || !result.marks) return [];
    const displayed = projectBeatMarks(result.marks, visible.first, pixelsPerSample, width, originX);
    if (previous?.length === displayed.length && displayed.every(({ index, depth, x }, i) =>
        index === previous[i].index && depth === previous[i].depth && x === previous[i].x)) return previous;
    return displayed;
});
function openBeatReader(): SeekBeatReader | undefined {
    const node = observedTimeline.value;
    const mode = gridMode.value;
    if (!observations || !node || mode === "seconds") return;
    const id = observations.portObservationId();
    let requested: ObservationWindow | undefined;
    let cache: { raw: NonNullable<ObservationWindowResult["marks"]>; first: bigint; length: bigint; targets: bigint[] } | undefined;
    const close = () => {
        requested = undefined;
        observations.observePort(id, undefined, "viewport");
    };
    return { close, marks({ first, length }, sampleRate, width) {
        if (mode !== gridMode.value || sampleRate !== scope.value.sampleRate || !observedTimeline.value
            || addressKey(node) !== addressKey(observedTimeline.value)) {
            close();
            return;
        }
        const window = timelineBeatObservationWindow(first, length, width);
        if (!window) return;
        if (!sameObservationWindow(window, requested)) {
            requested = window;
            observations.observePort(id, {
                source: { node, kind: "input", member: "beats", portKind: "event" }, operation: "beat-grid", window,
            }, "viewport");
        }
        const result = observations.windowForDisplay(id, window);
        const viewport = result?.status !== "ready" && timelineInterest.value
            ? observations.windowForDisplay(timelineInterest.value, observedTimelineWindow.value) : undefined;
        const raw = result?.status === "ready" ? result.marks : viewport?.status === "ready" ? viewport.marks : undefined;
        if (!raw) return result?.status === "ready" || result?.status === "unavailable" ? [] : undefined;
        if (cache?.raw !== raw || cache.first !== first || cache.length !== length) {
            const targets = projectBeatMarks(raw, first, width / Number(length), width).map(({ index }) => index);
            cache = { raw, first, length, targets };
        }
        return result?.status === "ready" || cache.targets.length ? cache.targets : undefined;
    } };
}
provideTimelineMarks(timelineMarks);
const beatStrengths = computed(() => gridGuideStrengths(timelineMarks.value, ({ x }) => x));
const valueNavigationTargets = new Map<Element, LaneValueNavigation>();
provideTimelineNavigation({ wheel, beginMiddleDrag, registerValueNavigation(element, lane) {
    valueNavigationTargets.set(element, lane);
    return () => { valueNavigationTargets.delete(element); };
} });
const middleDrag = shallowRef<MiddleDrag | null>(null);
const wrappedCursor = shallowRef<(Point & { action: MiddleDrag["action"] | "valueZoom" }) | null>(null);
let laneValueGesture: { lane: Element; gesture: LaneValueGesture } | null = null;
let laneHorizontalZoom: LaneHorizontalZoomGesture | null = null;
let lockedTravel: Point = { x: 0, y: 0 };
let middlePress: MiddleClick | null = null;
let lastMiddleClick: MiddleClick | null = null;
let middleCursorAnchor: (CursorWarpAnchor & { moved: boolean }) | null = null;
let middlePointerLockError = "";
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
    finishPointerLock(release, lockOrigin, middleCursorAnchor?.moved
        ? { ...middleCursorAnchor, destination: () => destination, policy: "wrapped" } : undefined, middlePointerLockError);
    middleCursorAnchor = null;
    finishMiddleDrag(id);
}, (message) => {
    middlePointerLockError = message;
    // If the browser rejects the request after taking capture away, end the drag.
    const drag = middleDrag.value;
    if (drag && !root.value?.hasPointerCapture(drag.pointerId)) endMiddleDrag();
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
    const contentTop = (root.value?.getBoundingClientRect().top ?? 0) + headerHeight.value;
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
        const horizontal = action !== "verticalZoom", vertical = action !== "horizontalZoom";
        viewport.value = zoomTimelineAxes(viewport.value, {
            x: horizontal ? zoomAnchor("horizontalZoom", event.clientX, event.clientY) : 0,
            y: vertical ? zoomAnchor("verticalZoom", event.clientX, event.clientY) : 0,
        }, delta, { horizontal, vertical });
        if (vertical) void applyScroll();
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
    if (document.pointerLockElement && document.pointerLockElement !== surface) return;
    try {
        surface.setPointerCapture(event.pointerId);
    } catch (error) {
        if (error instanceof DOMException && error.name === "NotFoundError") return;
        throw error;
    }
    event.preventDefault();
    event.stopPropagation();
    const lane = event.shiftKey && event.target instanceof Element ? event.target.closest(".lane") : null;
    const navigation = lane && valueNavigationTargets.get(lane);
    const mode = middleClickMode(event, !!navigation);
    middlePress = { x: event.clientX, y: event.clientY, at: performance.now(), mode, lane: mode === "lane" ? lane : null };
    const projectAction = timelineMiddleDragAction(event);
    const combined = projectAction === "bothZoom";
    const laneMode = event.altKey ? "zoom" : "pan";
    const gesture = (combined || mode === "lane") && navigation ? navigation.begin(laneMode) : null;
    if (lane && gesture) laneValueGesture = { lane, gesture };
    laneHorizontalZoom = combined && navigation ? navigation.beginHorizontalZoom?.() ?? null : null;
    const action = !combined && laneValueGesture ? "pan" : projectAction;
    const cursor = { x: event.clientX, y: event.clientY };
    const pointer = { ...cursor, pointerId: event.pointerId };
    middleDrag.value = action === "bothZoom" ? { ...pointer, action, zoomPickup: middleZoomPickup(viewport.value, {
        x: zoomAnchor("horizontalZoom", cursor.x, cursor.y),
        y: zoomAnchor("verticalZoom", cursor.x, cursor.y),
    }, cursor) } : { ...pointer, action };
    middleDragMoved = false;
    middlePointerLockError = "";
    const cursorAnchor = cursorWarpAnchor(event, window.devicePixelRatio);
    middleCursorAnchor = cursorAnchor ? { ...cursorAnchor, moved: false } : null;
    showMiddleZoomHint(action, { x: 0, y: 0 });
    middlePointerLock.start(event.pointerId, { x: event.clientX, y: event.clientY }, event.pointerType === "mouse");
}
function updateMiddleDrag(event: PointerEvent) {
    const drag = middleDrag.value;
    if (!drag || event.pointerId !== drag.pointerId || middlePointerLock.status().locked) return;
    if (!(event.buttons & 4)) { endMiddleDrag(); return; }
    const delta = { x: event.clientX - drag.x, y: event.clientY - drag.y };
    if (!delta.x && !delta.y) return;
    cancelMiddleClick();
    const { action } = drag;
    if (middleCursorAnchor) middleCursorAnchor.moved = true;
    middleDrag.value = { ...drag, x: event.clientX, y: event.clientY };
    if (action === "bothZoom") {
        applyCombinedMiddleZoom(delta, drag.zoomPickup);
        return;
    }
    const anchor = { x: action === "horizontalZoom" ? zoomAnchor(action, event.clientX, event.clientY) : 0,
        y: action === "verticalZoom" ? zoomAnchor(action, event.clientX, event.clientY) : 0 };
    const moved = moveMiddleDrag(viewport.value, drag, event.buttons, event.clientX, event.clientY, anchor, laneValueGesture !== null);
    if (moved && laneValueGesture?.gesture.mode !== "zoom") applyMiddleDrag(moved);
    moveLaneValueDrag(delta, event.clientY);
}
function applyCombinedMiddleZoom(delta: Point, pickup: MiddleZoomPickup) {
    const laneVertical = laneValueGesture?.gesture;
    if (delta.x) laneHorizontalZoom?.zoom(middleDragZoomFactor(delta.x), pickup.cursor.x);
    if (delta.y && laneVertical?.mode === "zoom") laneVertical.zoom(middleDragZoomFactor(delta.y), pickup.cursor.y);
    applyMiddleDrag(zoomMiddleDragAtPickup(viewport.value, delta, pickup, {
        horizontal: !laneHorizontalZoom, vertical: laneVertical?.mode !== "zoom",
    }));
}
function showMiddleZoomHint(action: MiddleDrag["action"], screens: Point) {
    const valueZoom = laneValueGesture?.gesture.mode === "zoom";
    if (action === "bothZoom") {
        setTimelineZoomHint([zoomAnchorHint("horizontalZoom", screens), zoomAnchorHint(valueZoom ? "valueZoom" : "verticalZoom", screens)].join(" · "));
    } else if (valueZoom) setTimelineZoomHint(zoomAnchorHint("valueZoom", screens));
    else if (action !== "pan") setTimelineZoomHint(zoomAnchorHint(action, screens));
}
function moveLaneValueDrag(delta: Point, cursorY: number) {
    const gesture = laneValueGesture?.gesture;
    if (gesture?.mode === "zoom" && delta.x) gesture.zoom(middleDragZoomFactor(delta.x), cursorY);
    if (gesture?.mode === "pan" && delta.y) gesture.pan(delta.y);
}
function wrappedPosition(point: Point, travel: Point) {
    const surface = root.value!;
    const rect = surface.getBoundingClientRect();
    const left = Math.max(0, rect.left + surface.clientLeft);
    const top = Math.max(0, rect.top + surface.clientTop);
    const right = Math.min(window.innerWidth, rect.left + surface.clientLeft + surface.clientWidth);
    const bottom = Math.min(window.innerHeight, rect.top + surface.clientTop + surface.clientHeight);
    if (middleDrag.value?.action === "bothZoom") {
        const x = laneHorizontalZoom?.content.getBoundingClientRect();
        const y = laneValueGesture?.lane.getBoundingClientRect();
        const localLeft = x ? Math.max(left, x.left) : left;
        const localRight = x ? Math.min(right, x.right) : right;
        const localTop = y ? Math.max(top, rect.top + surface.clientTop + headerHeight.value, y.top) : top;
        const localBottom = y ? Math.min(bottom, y.bottom) : bottom;
        return wrapTimelineCursor(point, travel, {
            left: localRight > localLeft ? localLeft : left, width: localRight > localLeft ? localRight - localLeft : right - left,
            top: localBottom > localTop ? localTop : top, height: localBottom > localTop ? localBottom - localTop : bottom - top,
        });
    }
    if (laneValueGesture) {
        // The sticky ruler covers part of a lane that has scrolled past it.
        const { lane: element, gesture } = laneValueGesture;
        const { mode } = gesture;
        const lane = (mode === "pan" ? gesture.content : element).getBoundingClientRect();
        const laneLeft = Math.max(left, lane.left);
        const laneTop = Math.max(top, rect.top + surface.clientTop + headerHeight.value, lane.top);
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
    cancelMiddleClick();
    const { action } = drag;
    if (action !== "bothZoom" && laneValueGesture?.gesture.mode === "zoom") {
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
    if (action === "bothZoom") {
        applyCombinedMiddleZoom(delta, drag.zoomPickup);
        return;
    }
    // Reconstruct the virtual position from the start and total travel. Only
    // the glyph wraps. Neither zoom anchor is calculated from the glyph.
    showMiddleZoomHint(action, screens);
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
function endMiddleDrag(event?: PointerEvent) {
    const pointerId = middleDrag.value?.pointerId;
    if (pointerId === undefined || (event && event.pointerId !== pointerId)) return;
    cancelMiddleClick();
    middlePointerLock.cancel();
}
function middleCaptureLost(event: PointerEvent) {
    if (event.target === root.value && !middlePointerLock.suppressPointerCancellation()) endMiddleDrag(event);
}
function middlePointerCancelled(event: PointerEvent) {
    if (event.target === root.value) endMiddleDrag(event);
}
function finishMiddleDrag(pointerId: number) {
    const drag = middleDrag.value;
    if (drag?.pointerId !== pointerId) return;
    if (middlePress) cancelMiddleClick();
    middleDrag.value = null;
    // Keep the wrapped cursor visible until the browser restores its cursor.
    if (document.pointerLockElement !== root.value) wrappedCursor.value = null;
    if (drag.action !== "pan" || laneValueGesture?.gesture.mode === "zoom") setTimelineZoomHint(null);
    const laneChanged = laneValueGesture?.gesture.changed() || laneHorizontalZoom?.changed();
    laneValueGesture = null;
    laneHorizontalZoom = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!middleDragMoved && !laneChanged) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
function middleLockChanged() {
    middlePointerLock.changed();
    const drag = middleDrag.value;
    if (!drag && document.pointerLockElement !== root.value) wrappedCursor.value = null;
    if (!drag || !root.value || !middlePointerLock.status().granted) return;
    lockedTravel = { x: 0, y: 0 };
    const { visible, screens } = wrappedPosition(drag, lockedTravel);
    wrappedCursor.value = { ...visible, action: drag.action !== "bothZoom" && laneValueGesture?.gesture.mode === "zoom" ? "valueZoom" : drag.action };
    if (drag.action !== "bothZoom") showMiddleZoomHint(drag.action, screens);
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
        cancelMiddleClick();
        middlePointerLock.release(drag.pointerId, { x: event.clientX, y: event.clientY });
        event.stopImmediatePropagation();
    } else if (middlePointerLock.move(delta)) event.stopImmediatePropagation();
}
function releaseMiddleDrag(event: MouseEvent) {
    const drag = middleDrag.value;
    if (!drag || event.button !== 1) return;
    if ("pointerId" in event && event.pointerId !== drag.pointerId) return;
    try {
        completeMiddleClick();
    } finally {
        middlePointerLock.release(drag.pointerId, { x: event.clientX, y: event.clientY });
    }
}
function nativeFocusChanged(focused: boolean) {
    if (!focused) {
        cancelMiddleClick();
        endMiddleDrag();
    }
}
function middleVisibilityChanged() {
    if (document.hidden) {
        cancelMiddleClick();
        endMiddleDrag();
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
    if (middleDrag.value) endMiddleDrag();
    if (saveTimer) { clearTimeout(saveTimer); save(); }
});
</script>

<template>
    <section ref="root" class="timeline-scope" tabindex="-1"
        :data-middle-drag="middleDrag && (middleDrag.action !== 'bothZoom' && laneValueGesture?.gesture.mode === 'zoom' ? 'valueZoom' : middleDrag.action)" aria-label="Project timeline"
        :style="scrollbarStyle" :data-has-selection="selectedLanes.size > 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown.capture="beginMiddleDrag" @pointermove="updateMiddleDrag"
        @pointercancel="middlePointerCancelled" @lostpointercapture="middleCaptureLost"
        @mousedown.capture="preventMiddleDefault" @auxclick.capture="preventMiddleDefault">
        <header class="timeline-heading">
            <div class="node-column">
                <div class="playback-position" data-status-shortcuts="Space ▶/Ⅱ">
                    <template v-if="backgroundPlayheadSeconds !== null">
                        {{ playback.state.value?.running ? '▷' : 'Ⅱ' }} {{ backgroundPlayheadSeconds.toFixed(3) }} s
                        <small>{{ playback.backgroundPlayhead.value?.toString() }} smp</small>
                    </template>
                </div>
                <div v-if="gridMode === 'custom'" class="timeline-snap-input">
                    <NodePorts v-if="customInput?.ports.length" :node="customInput" :inline="false" />
                    <span v-else role="status">Loading Beats input…</span>
                </div>
            </div>
            <div ref="timeRegion" class="time-region">
                <TimeRuler :window="timeWindow" :marks="timelineMarks" :beat-strengths="beatStrengths"
                    :open-beat-reader="openBeatReader" playback style="height: 100%" />
                <LaneSettings :label="gridLabel" dialog-label="Timeline snap mode"
                    hint="Choose the timeline grid and snapping" :width="240" :preferred-height="settingsGrid ? 300 : 104" v-slot="{ close }">
                    <div class="timeline-grid-choices">
                        <button v-for="{ mode, label, description } in gridModes" :key="mode" type="button" tabindex="-1"
                            :disabled="!scope.editable" :aria-pressed="gridMode === mode" @click="setGridMode(mode)">
                            {{ label }} <span>{{ description }}</span>
                        </button>
                    </div>
                    <BeatsSettings v-if="gridMode === 'beats' && scope.editable && beatsNode && settingsGrid"
                        :target="beatsNode.address" :grid="settingsGrid" @done="close" />
                </LaneSettings>
            </div>
        </header>
        <div class="lanes">
            <template v-if="gridMode !== 'seconds'">
                <svg v-for="run in timelineRuns" :key="run.begin" class="lane-timeline-overlay lane-snap-grid" aria-hidden="true"
                    :style="run.style">
                    <TransitionGroup name="iv-grid">
                        <line v-for="mark in timelineMarks" :key="mark.index.toString()" class="iv-grid-guide"
                            :style="{ '--iv-grid-strength': beatStrengths.get(mark.depth) }"
                            :x1="mark.x" :x2="mark.x" y1="0" y2="100%" vector-effect="non-scaling-stroke" />
                    </TransitionGroup>
                </svg>
            </template>
            <section v-for="{ child, semantic } in children" :key="addressKey(child.address)"
                class="lane" :class="{ selected: selectedLanes.has(addressKey(child.address)) }"
                :data-lane-key="addressKey(child.address)"
                :style="{ height: `${laneHeight}px` }" tabindex="-1"
                :aria-label="nodeDisplayName(semantic) + (selectedLanes.has(addressKey(child.address)) ? ', selected' : '')">
                <PresentationHost :node="semantic" :region-height="laneHeight - 1" lane
                    :rename-lane="scope.editable ? (name, previousName) => renameLane(child.instance.instanceId, name, previousName) : undefined"
                    @copy-debug-details="copyNodeDebugDetails(child.address)" />
            </section>
            <svg v-for="run in timelineRuns" :key="run.begin" class="lane-timeline-overlay" aria-hidden="true"
                :style="run.style">
                <TimelinePlayhead />
            </svg>
            <div v-if="!children.length" class="empty">No project instances.</div>
        </div>
    </section>
    <Teleport to="body">
        <svg v-if="wrappedCursor" class="wrapped-cursor" :style="{ left: `${wrappedCursor.x}px`, top: `${wrappedCursor.y}px` }"
            viewBox="0 0 20 20" aria-hidden="true">
            <path :transform="wrappedCursor.action === 'verticalZoom' || wrappedCursor.action === 'valueZoom' ? 'rotate(90 10 10)' : undefined"
                d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
            <path v-if="wrappedCursor.action === 'pan' || wrappedCursor.action === 'bothZoom'" transform="rotate(90 10 10)"
                d="M10 3v14M7 6l3-3 3 3m-6 8 3 3 3-3" />
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
.timeline-scope[data-middle-drag="bothZoom"] { cursor: move; }
.timeline-scope[data-middle-drag="horizontalZoom"] { cursor: ns-resize; }
.timeline-scope:is([data-middle-drag="verticalZoom"], [data-middle-drag="valueZoom"]) { cursor: ew-resize; }
.wrapped-cursor {
    position: fixed; z-index: 1000; width: 20px; height: 20px; transform: translate(-50%, -50%);
    pointer-events: none; fill: none; stroke: var(--vscode-editor-foreground); stroke-width: 1.5;
    stroke-linecap: round; stroke-linejoin: round; filter: drop-shadow(0 0 1px var(--vscode-editor-background));
}
.time-region { position: relative; min-width: 0; }
.timeline-grid-choices { display: grid; gap: 2px; }
.timeline-grid-choices button { border: 0; padding: 4px 6px; text-align: left; font: inherit;
    font-size: 11px; border-radius: 3px; background: transparent; color: inherit; cursor: pointer; }
.timeline-grid-choices button span { display: block; font-size: 10px; opacity: .8; }
.timeline-grid-choices button[aria-pressed="true"] { background: var(--vscode-list-activeSelectionBackground);
    color: var(--vscode-list-activeSelectionForeground); }
.timeline-grid-choices button:not([aria-pressed="true"]):hover { background: var(--vscode-list-hoverBackground); }
.timeline-heading { display: grid; grid-template-columns: var(--node-label-width) minmax(0, 1fr); }
.timeline-heading { box-sizing: border-box; position: sticky; top: 0; z-index: 1; background: var(--vscode-editor-background); flex: 0 0 var(--timeline-header-height); border-bottom: 1px solid var(--vscode-panel-border); }
.node-column { min-width: 0; border-right: 1px solid var(--vscode-panel-border); }
.timeline-snap-input { padding-inline: 12px; }
.timeline-snap-input :deep(.node-ports) { height: var(--port-row-height); margin: 0; }
.lanes { flex: 0 0 auto; min-height: 0; position: relative; isolation: isolate; }
.lane-timeline-overlay { position: absolute; inset-inline-start: var(--node-label-width);
    width: calc(100% - var(--node-label-width)); pointer-events: none; }
.lane-snap-grid { z-index: -1; }
.playback-position { box-sizing: border-box; height: 32px; padding: 8px 12px 0; overflow: hidden;
    white-space: nowrap; font-variant-numeric: tabular-nums; color: var(--iv-lane-title-color); }
.playback-position small { color: var(--iv-port-label-base); margin-left: 6px; }
.lane { position: relative; box-sizing: border-box; border-bottom: 1px solid transparent; }
/* The snap grid passes over lane separators and under their contents. */
.lane::before {
    content: ""; position: absolute; z-index: -2; pointer-events: none;
    inset-inline: 0; inset-block-end: -1px; height: 1px; background: var(--vscode-panel-border);
}
.lane.selected::after {
    content: ""; position: absolute; z-index: 0; pointer-events: none;
    inset-inline-start: calc(var(--node-label-width) - 1px); inset-block: 0; width: 2px;
    background: var(--vscode-tab-activeBorderTop, var(--vscode-tab-activeBorder,
        var(--vscode-panelTitle-activeBorder, var(--vscode-focusBorder))));
}
.lane.selected:has(+ .lane.selected)::after { inset-block-end: -1px; }
.empty { padding: 12px; }
</style>

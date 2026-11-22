<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch } from "vue";
import { addressKey, formatLaneLabel, isProjectInstance, laneLabels, type NodeAddress, type Point } from "../../src/graphScope";
import { isGeneratedSnapConnection, isTimelineSnapAddress, managedBeats, managedTimeline, timelineGridMode,
    type TimelineGridMode } from "../../src/timelineSnap";
import { laneRuns } from "../../src/laneSelection";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { listenPortConnectionFocus } from "../../src/portConnectionFocus";
import { useNode } from "../presentation/nodeContext";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import PresentationHost from "../presentation/PresentationHost.vue";
import { selectPresentation } from "../presentation/presentations";
import { nodeDisplayName } from "../presentation/nodeSummary";
import LaneSettings from "../presentation/LaneSettings.vue";
import BeatsSettings from "../presentation/BeatsSettings.vue";
import NodePorts from "../presentation/NodePorts.vue";
import { useBeatGridEditing } from "../presentation/beatGridEditingContext";
import TimeRuler from "../timeline/TimeRuler.vue";
import GridGuides from "../timeline/GridGuides.vue";
import { usePresentationFrame } from "../presentation/presentationFrames";
import { usePresentationSize } from "../presentation/presentationSize";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import type { SeekBeatReader } from "../timeline/usePlayheadSeek";
import { timelineBeatObservationWindow } from "../../src/timelineGeometry";
import { beatGridTicks, beatRulerLabels, minimumBeatMarkSpacing, readBeatGrid, sameBeatGrid, selectVisibleBeatMarks,
    type BeatGrid } from "../../src/beatGrid";
import { gridGuideStrengths } from "../../src/gridGuideStrength";
import { provideTimelineMarks, provideTimelineNavigation, provideTimelineWindow, timelineMarkKey, timelineMarkPosition, type TimelineMark,
    type LaneHorizontalZoomGesture, type LaneValueGesture, type LaneValueNavigation } from "../timeline/timelineContext";
import { editableHeightPx, formatPlaybackSeconds, initialTimelineViewport, isMiddleDoubleClick, middleClickMode, middleDragZoomFactor, middleZoomPickup, moveMiddleDrag,
    panTimeline, resetTimelineZoom, timelineMiddleDragAction, timelineWheelAction, wheelPixels, wrapTimelineCursor,
    zoomAnchorHint, zoomFactorForPixels, zoomMiddleDrag, zoomMiddleDragAtPickup, zoomTimelineHorizontally,
    stepTimelineVerticalZoom, timelineLaneBorderPx, verticalWheelStepPixels,
    type MiddleClick, type MiddleDrag, type MiddleZoomPickup, type TimelineZoomAction, type TimelineViewport } from "../../src/timelineViewport";
import { useGraphEditor } from "./context";
import { cursorWarpAnchor, type CursorWarpAnchor } from "./cursorReturn";
import { usePlayback } from "../timeline/playbackContext";
import { useObservations, usePortObservationViewport } from "../presentation/observationContext";
import { predictedObservationRows } from "../../src/observationWindows";
import { sameObservationWindow, type ObservationWindow, type ObservationWindowResult } from "../../src/observations";
import { automationSampleIndex, automationSampleOffset } from "../../src/automationCurveGeometry";
import { webviewApi } from "../host";

const node = useNode();
const playback = usePlayback();
const backgroundPlayheadSeconds = computed(() => playback.backgroundPlayhead.value === null ? null
    : Number(playback.backgroundPlayhead.value) / (playback.state.value?.sampleRate ?? 48000));
const scope = computed(() => node.value.scope!);
const { layout, lanes, save, copyNodeDebugDetails, selectedLanes, expandedModules, finishPointerLock, setTimelineZoomHint,
    profileGraphUi, portsChanged, startObjectDrag } = useGraphEditor();
function renameLane(instanceId: string, displayName: string, previousDisplayName: string) {
    webviewApi.postMessage({ type: "renameGraphLane", instanceId, displayName, previousDisplayName });
}
function openVirtualNodeSource(address: NodeAddress) {
    webviewApi.postMessage({ type: "openVirtualNodeSource", nodeKey: addressKey(address) });
}
const children = computed(() => profileGraphUi("Build lane presentation data", () => {
    const boundaries = new Set(scope.value.childScopes?.filter((scope) => scope.hasBoundary).map(({ subject }) => addressKey(subject)));
    return lanes.value.flatMap(({ node: child, depth, rowKey }) => {
        const address = addressKey(child.address);
        const semantic = semanticNodeFromScopeChild(child, scope.value);
        const lane = { child, depth, rowKey, semantic, moduleBoundary: false };
        if (!expandedModules.value.has(address) || !boundaries.has(address) || !semantic.ports.length) return [lane];
        return [lane, { ...lane, depth: depth + 1, rowKey: `${rowKey}/module-ports`, moduleBoundary: true }];
    });
}));
const laneOrder = computed(() => children.value.map(({ child, moduleBoundary, rowKey }) =>
    moduleBoundary ? rowKey : addressKey(child.address)));
const viewport = computed({ get: () => layout.value.timeline,
    set: (value) => { layout.value.timeline = value; } });
let pendingMiddleViewport: TimelineViewport | undefined;
const middleViewportFrame = usePresentationFrame("update", commitMiddleViewport);

function queueMiddleViewport(next: TimelineViewport) {
    const current = pendingMiddleViewport ?? viewport.value;
    if (next === current) return;
    // Apply the scroll bound to each input before composing the next one.
    // Reversing direction at the last lane can then move immediately.
    if (root.value && (next.scrollTop !== current.scrollTop || next.laneHeight !== current.laneHeight)) {
        const bottom = headerHeight.value + children.value.length * next.laneHeight;
        const maxScroll = Math.max(0, bottom - root.value.clientHeight);
        if (next.scrollTop > maxScroll) next = { ...next, scrollTop: maxScroll };
    }
    pendingMiddleViewport = next;
    middleViewportFrame.schedule();
}

function commitMiddleViewport() {
    middleViewportFrame.cancel();
    const next = pendingMiddleViewport;
    pendingMiddleViewport = undefined;
    if (next) applyViewport(next);
    wrappedCursor.value = latestWrappedCursor;
}
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
const { edits: beatEdits, version: beatEditsVersion } = useBeatGridEditing();
async function setGridMode(mode: TimelineGridMode) {
    if (!scope.value.editable || gridMode.value === mode) return;
    if (beatsNode.value) await beatEdits.finish(beatsNode.value.address);
    webviewApi.postMessage({ type: "setTimelineSnapMode", mode });
}
const laneHeight = computed(() => viewport.value.laneHeight);
const laneContentHeight = computed(() => laneHeight.value - timelineLaneBorderPx);
const nestedAreas = computed(() => {
    const rows = children.value;
    type Area = { key: string; begin: number; end: number };
    const areas: Area[] = [];
    const open: { depth: number; area: Area }[] = [];
    for (const [index, { depth, rowKey }] of rows.entries()) {
        while (open.length && open[open.length - 1].depth >= depth) {
            open.pop()!.area.end = index;
        }
        if ((rows[index + 1]?.depth ?? 0) <= depth) continue;
        const area = { key: rowKey, begin: index + 1, end: rows.length };
        areas.push(area);
        open.push({ depth, area });
    }
    return areas;
});
const nestedAreaEnds = computed(() => [...new Set(nestedAreas.value.map(({ end }) => end))]);
const timelineRuns = computed(() => {
    const timelineLanes = new Set(children.value.filter(({ semantic, moduleBoundary }) =>
        !moduleBoundary && selectPresentation(semantic.definitionId).timeline)
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
const playbackReadout = ref<HTMLElement | null>(null);
const timelineSize = usePresentationSize(root);
const timeRegionSize = usePresentationSize(timeRegion);
const playbackReadoutSize = usePresentationSize(playbackReadout);
const minimumHeaderWidth = 74;
const playbackSymbol = computed(() => playback.state.value?.running ? "▷" : "Ⅱ");
let playbackTextContext: CanvasRenderingContext2D | null = null;
const measurePlaybackText = computed(() => {
    const element = playbackReadout.value;
    const width = playbackReadoutSize.value?.width;
    if (!element || width === undefined) return;
    const style = getComputedStyle(element);
    const context = playbackTextContext ??= document.createElement("canvas").getContext("2d");
    if (!context) return;
    context.font = `${style.fontStyle} ${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
    const digitWidth = Math.max(...Array.from("0123456789", (digit) => context.measureText(digit).width));
    return (text: string) => {
        const digits = text.match(/\d/g)?.length ?? 0;
        return context.measureText(text.replace(/\d/g, "")).width + digits * digitWidth <= width;
    };
});
const playbackSecondsLabel = computed(() => {
    const seconds = backgroundPlayheadSeconds.value;
    if (seconds === null) return null;
    return formatPlaybackSeconds(seconds, (text) => measurePlaybackText.value?.(`${playbackSymbol.value} ${text} s`) ?? true);
});
const maximumHeaderWidth = computed(() => {
    const timelineWidth = timelineSize.value?.width ?? Infinity;
    return Math.max(minimumHeaderWidth, Math.floor(Math.min(260, timelineWidth * 0.4)));
});
const timeHeaderWidth = shallowRef(webviewApi.getState()?.timelineHeaderWidth);
function resizeTimelineHeaders(event: PointerEvent) {
    if (event.button !== 0 || !root.value || !timeRegion.value) return;
    const startX = event.clientX;
    const initial = timeRegion.value.getBoundingClientRect().left - root.value.getBoundingClientRect().left;
    const previous = timeHeaderWidth.value;
    const move = ({ x }: Point) => {
        if (!root.value || (x === startX && timeHeaderWidth.value === previous)) return;
        const width = Math.round(Math.max(minimumHeaderWidth,
            Math.min(maximumHeaderWidth.value, initial + x - startX)));
        if (width === timeHeaderWidth.value) return;
        timeHeaderWidth.value = width;
        void nextTick(portsChanged);
    };
    if (startObjectDrag(event, {
        lock: false, move, moveBy: () => {},
        finish(point): undefined {
            if (point) {
                move(point);
                webviewApi.setState({ ...webviewApi.getState(), timelineHeaderWidth: timeHeaderWidth.value });
            } else {
                timeHeaderWidth.value = previous;
                void nextTick(portsChanged);
            }
        },
    })) event.preventDefault();
}
const timeWidth = computed(() => timeRegionSize.value?.width ?? 0);
const timeWindow = computed(() => {
    const { beginSeconds, secondsPerPixel } = viewport.value;
    const spanSeconds = Math.max(1, timeWidth.value) * secondsPerPixel;
    return { beginSeconds, endSeconds: beginSeconds + spanSeconds, spanSeconds, sampleRate: scope.value.sampleRate,
        gridMode: gridMode.value };
});
provideTimelineWindow(timeWindow);
const observations = useObservations();
const observedTimeline = computed(() => scope.value.timelineNodes?.find(isTimelineSnapAddress));
const decodedBeatMarks = new WeakMap<NonNullable<ObservationWindowResult["marks"]>, Array<Pick<TimelineMark, "index" | "depth">>>();
const beatViewport = computed(() => {
    const { beginSeconds, spanSeconds, sampleRate } = timeWindow.value;
    if (gridMode.value === "seconds" || !sampleRate || timeWidth.value <= 0) return undefined;
    const first = automationSampleIndex(beginSeconds, sampleRate);
    const last = automationSampleIndex(beginSeconds, sampleRate, spanSeconds);
    return first === undefined || last === undefined || last < first ? undefined : { first, length: last - first + 1n };
});
const visibleBeatWindow = computed<ObservationWindow | undefined>((previous) => {
    const visible = beatViewport.value;
    if (!visible) return;
    const window = timelineBeatObservationWindow(visible.first, visible.length, timeWidth.value, false);
    return sameObservationWindow(window, previous) ? previous : window;
});
const beatWindows = usePortObservationViewport(() => {
    const node = observedTimeline.value, window = visibleBeatWindow.value;
    if (!node || !window) return;
    return { source: { node, kind: "input", member: "beats", portKind: "event" }, operation: "beat-grid",
        window, indexedEvents: gridMode.value === "beats", consumer: "Timeline ruler" };
});
const canPreviewBeatGrid = computed(() => {
    if (gridMode.value !== "beats" || !snapTimeline.value) return false;
    const inputs = scope.value.connections.filter(({ connection }) =>
        connection.kind === "event" && connection.inputs.some(isTimelineSnapAddress));
    return inputs.length === 1 && isGeneratedSnapConnection(inputs[0]);
});
const previewGrid = computed<BeatGrid | undefined>((previous) => {
    const node = beatsNode.value, accepted = settingsGrid.value;
    if (!node || !accepted || !canPreviewBeatGrid.value) return;
    void beatEditsVersion.value;
    const draft = beatEdits.draft(node.address);
    if (draft) return draft;
    // Scope can acknowledge the edit before the receiving port delivers the
    // new window. Keep displaying the accepted preview over retained old data.
    const observed = beatWindows.value?.result;
    if (previous && sameBeatGrid(previous, accepted) && (!observed || observed.status === "pending" || observed.outdated))
        return previous;
    return undefined;
});
const observedBeatMarks = computed<NonNullable<ObservationWindowResult["marks"]>>((previous) => {
    const window = visibleBeatWindow.value, observation = beatWindows.value;
    if (!window || !observation) return [];
    const origin = BigInt(window.begin);
    const marks = observation.spans.flatMap(({ result, begin, end }) =>
        (result.marks ?? []).filter(({ index }) => {
            const offset = BigInt.asIntN(64, BigInt(index) - origin);
            return begin <= offset && offset < end;
        }));
    if (previous?.length === marks.length && marks.every((mark, index) => mark === previous[index])) return previous;
    return marks;
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
const timelineMarks = computed<readonly TimelineMark[]>((previous) => profileGraphUi("Project timeline beat grid", () => {
    if (gridMode.value === "seconds") return [];
    const { beginSeconds, spanSeconds, sampleRate } = timeWindow.value;
    const visible = beatViewport.value;
    const width = timeWidth.value;
    if (!sampleRate || !visible) return [];
    const draft = previewGrid.value;
    let displayed: TimelineMark[];
    if (draft) {
        displayed = beatGridTicks(draft, { beginSeconds, spanSeconds }, sampleRate, width)
            .map(({ sample, depth, x }) => ({ index: sample, depth, x }));
    } else {
        if (!observedTimeline.value) return [];
        const pixelsPerSample = width / (spanSeconds * sampleRate);
        const originX = automationSampleOffset(visible.first, beginSeconds, sampleRate) / spanSeconds * width;
        displayed = projectBeatMarks(observedBeatMarks.value, visible.first, pixelsPerSample, width, originX);
    }
    if (previous?.length === displayed.length && displayed.every(({ index, depth, x }, i) =>
        index === previous[i].index && depth === previous[i].depth && x === previous[i].x)) return previous;
    return displayed;
}));
function openBeatReader(): SeekBeatReader | undefined {
    const node = observedTimeline.value;
    const mode = gridMode.value;
    const draft = previewGrid.value;
    if (draft) {
        let cached: { first: bigint; length: bigint; targets: bigint[] } | undefined;
        return { marks({ first, length }, sampleRate, width) {
            if (mode !== gridMode.value || sampleRate !== scope.value.sampleRate) return;
            if (cached?.first !== first || cached.length !== length) {
                const targets = beatGridTicks(draft, { beginSample: first, spanSeconds: Number(length) / sampleRate }, sampleRate, width)
                    .map(({ sample }) => sample);
                cached = { first, length, targets };
            }
            return cached.targets;
        } };
    }
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
        const window = timelineBeatObservationWindow(first, length, width, mode === "beats");
        if (!window) return;
        if (!sameObservationWindow(window, requested)) {
            requested = window;
            observations.observePort(id, {
                source: { node, kind: "input", member: "beats", portKind: "event" }, operation: "beat-grid", window,
            }, "viewport", "Playhead snapping");
        }
        const result = observations.windowForDisplay(id, window);
        const raw = result?.status === "ready" ? result.marks : observedBeatMarks.value;
        if (!raw) return result?.status === "ready" || result?.status === "unavailable" ? [] : undefined;
        if (cache?.raw !== raw || cache.first !== first || cache.length !== length) {
            const targets = projectBeatMarks(raw, first, width / Number(length), width).map(({ index }) => index);
            cache = { raw, first, length, targets };
        }
        return result?.status === "ready" || cache.targets.length ? cache.targets : undefined;
    } };
}
provideTimelineMarks(timelineMarks);
const beatStrengths = computed(() => gridGuideStrengths(timelineMarks.value, timelineMarkPosition));
const beatLabels = computed(() => {
    const sampleRate = timeWindow.value.sampleRate;
    const grid = previewGrid.value ?? settingsGrid.value;
    if (gridMode.value !== "beats" || !grid || !sampleRate) return [];
    // Reserve room for the ruler's settings button.
    return beatRulerLabels(grid, timelineMarks.value, sampleRate, timeWidth.value - 56);
});
const valueNavigationTargets = new Map<Element, LaneValueNavigation>();
provideTimelineNavigation({ wheel, beginMiddleDrag, registerValueNavigation(element, lane) {
    valueNavigationTargets.set(element, lane);
    return () => { valueNavigationTargets.delete(element); };
} });
const middleDrag = shallowRef<MiddleDrag | null>(null);
type WrappedMiddleCursor = Point & { action: MiddleDrag["action"] | "valueZoom" };
const wrappedCursor = shallowRef<WrappedMiddleCursor | null>(null);
let latestWrappedCursor: WrappedMiddleCursor | null = null;
function moveWrappedCursor(next: WrappedMiddleCursor) {
    latestWrappedCursor = next;
    middleViewportFrame.schedule();
}
function clearWrappedCursor() {
    latestWrappedCursor = wrappedCursor.value = null;
}
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
    const destination = latestWrappedCursor;
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
let saveTimer: ReturnType<typeof setTimeout> | undefined;
let scrollRequest = 0;
let disposed = false;
let stopListeningForFocus: (() => void) | undefined;
let lastScroll: { top: number; at: number } | undefined;
let scrollSpeed = 0;

function predictObservationViewport() {
    const surface = root.value;
    if (!surface || !observations) return;
    const prediction = predictedObservationRows(surface.scrollTop, surface.clientHeight, headerHeight.value,
        laneHeight.value, children.value.length, scrollSpeed);
    observations.predictViewport(prediction ? children.value.slice(prediction.begin, prediction.end)
        .filter(({ moduleBoundary }) => !moduleBoundary).map(({ child }) => child.address) : []);
}
watch([children, laneHeight, headerHeight, timelineSize], predictObservationViewport, { flush: "post" });

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
    predictObservationViewport();
}
function scrolled() {
    if (!root.value) return;
    const top = root.value.scrollTop, at = performance.now();
    if (lastScroll && top !== lastScroll.top) scrollSpeed = (top - lastScroll.top) / Math.max(16, at - lastScroll.at);
    lastScroll = { top, at };
    viewport.value.scrollTop = top;
    predictObservationViewport();
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
    profileGraphUi("Handle timeline wheel", () => applyWheel(event));
}
let verticalWheelRemainder = 0;
function applyWheel(event: WheelEvent) {
    if (pendingMiddleViewport) commitMiddleViewport();
    const lane = event.shiftKey && event.target instanceof Element ? event.target.closest(".lane") : null;
    const navigation = lane && valueNavigationTargets.get(lane);
    const action = timelineWheelAction(event, !!navigation);
    if (action !== "verticalZoom" && action !== "bothZoom") verticalWheelRemainder = 0;
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
        if (horizontal)
            viewport.value = zoomTimelineHorizontally(viewport.value, zoomAnchor("horizontalZoom", event.clientX, event.clientY), delta);
        if (vertical) {
            verticalWheelRemainder += delta;
            const steps = Math.trunc(verticalWheelRemainder / verticalWheelStepPixels);
            verticalWheelRemainder -= steps * verticalWheelStepPixels;
            if (steps) {
                viewport.value = stepTimelineVerticalZoom(viewport.value, zoomAnchor("verticalZoom", event.clientX, event.clientY), steps);
                void applyScroll();
            }
        }
    }
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
    const moved = moveMiddleDrag(pendingMiddleViewport ?? viewport.value, drag, event.buttons, event.clientX, event.clientY, anchor,
        laneValueGesture !== null);
    if (moved && laneValueGesture?.gesture.mode !== "zoom") queueMiddleViewport(moved);
    moveLaneValueDrag(delta, event.clientY);
}
function applyCombinedMiddleZoom(delta: Point, pickup: MiddleZoomPickup) {
    const laneVertical = laneValueGesture?.gesture;
    if (delta.x) laneHorizontalZoom?.zoom(middleDragZoomFactor(delta.x), pickup.cursor.x);
    if (delta.y && laneVertical?.mode === "zoom") laneVertical.zoom(middleDragZoomFactor(delta.y), pickup.cursor.y);
    queueMiddleViewport(zoomMiddleDragAtPickup(pendingMiddleViewport ?? viewport.value, delta, pickup, {
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
    profileGraphUi("Handle timeline middle drag", () => applyLockedMiddleDrag(drag, delta));
}
function applyLockedMiddleDrag(drag: MiddleDrag, delta: Point) {
    if (middleCursorAnchor) middleCursorAnchor.moved = true;
    cancelMiddleClick();
    const { action } = drag;
    if (action !== "bothZoom" && laneValueGesture?.gesture.mode === "zoom") {
        lockedTravel.x += delta.x;
        lockedTravel.y += delta.y;
        const { visible, virtual, screens } = wrappedPosition(drag, lockedTravel);
        moveWrappedCursor({ ...visible, action: "valueZoom" });
        setTimelineZoomHint(zoomAnchorHint("valueZoom", screens));
        moveLaneValueDrag(delta, virtual.y);
        return;
    }
    if (action === "pan") {
        const { visible } = wrappedPosition(latestWrappedCursor ?? drag, delta);
        moveWrappedCursor({ ...visible, action });
        queueMiddleViewport(panTimeline(pendingMiddleViewport ?? viewport.value, delta, laneValueGesture !== null));
        moveLaneValueDrag(delta, visible.y);
        return;
    }
    lockedTravel.x += delta.x;
    lockedTravel.y += delta.y;
    const { virtual, visible, screens } = wrappedPosition(drag, lockedTravel);
    moveWrappedCursor({ ...visible, action });
    if (action === "bothZoom") {
        applyCombinedMiddleZoom(delta, drag.zoomPickup);
        return;
    }
    // Reconstruct the virtual position from the start and total travel. Only
    // the glyph wraps. Neither zoom anchor is calculated from the glyph.
    showMiddleZoomHint(action, screens);
    const anchor = zoomAnchor(action, virtual.x, virtual.y, true);
    queueMiddleViewport(zoomMiddleDrag(pendingMiddleViewport ?? viewport.value, action, delta, anchor));
}
function applyViewport(moved: TimelineViewport) {
    const current = viewport.value;
    if (moved === current) return;
    viewport.value = moved;
    if (moved.scrollTop !== current.scrollTop || moved.laneHeight !== current.laneHeight) void applyScroll();
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
    applyViewport(axis === "view" ? { ...initialTimelineViewport }
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
    commitMiddleViewport();
    if (middlePress) cancelMiddleClick();
    middleDrag.value = null;
    // Keep the wrapped cursor visible until the browser restores its cursor.
    if (document.pointerLockElement !== root.value) clearWrappedCursor();
    if (drag.action !== "pan" || laneValueGesture?.gesture.mode === "zoom") setTimelineZoomHint(null);
    const laneChanged = laneValueGesture?.gesture.changed() || laneHorizontalZoom?.changed();
    laneValueGesture = null;
    laneHorizontalZoom = null;
    if (root.value?.hasPointerCapture(pointerId)) root.value.releasePointerCapture(pointerId);
    if (!laneChanged) return;
    clearTimeout(saveTimer);
    saveTimer = undefined;
    save();
}
function middleLockChanged() {
    middlePointerLock.changed();
    const drag = middleDrag.value;
    if (!drag && document.pointerLockElement !== root.value) clearWrappedCursor();
    if (!drag || !root.value || !middlePointerLock.status().granted) return;
    lockedTravel = { x: 0, y: 0 };
    const { visible, screens } = wrappedPosition(drag, lockedTravel);
    moveWrappedCursor({ ...visible, action: drag.action !== "bothZoom" && laneValueGesture?.gesture.mode === "zoom" ? "valueZoom" : drag.action });
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
        commitMiddleViewport();
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
    observations?.predictViewport([]);
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
        :style="[scrollbarStyle, { '--preferred-header-width': timeHeaderWidth === undefined ? undefined : `${timeHeaderWidth}px`,
            '--minimum-header-width': `${minimumHeaderWidth}px`, '--maximum-header-width': `${maximumHeaderWidth}px`,
            '--lane-border-width': `${timelineLaneBorderPx}px` }]" :data-has-selection="selectedLanes.size > 0"
        @wheel="wheel" @scroll="scrolled" @pointerdown.capture="beginMiddleDrag" @pointermove="updateMiddleDrag"
        @pointercancel="middlePointerCancelled" @lostpointercapture="middleCaptureLost"
        @mousedown.capture="preventMiddleDefault" @auxclick.capture="preventMiddleDefault">
        <header class="timeline-heading">
            <div class="header-width-handle" role="separator" aria-orientation="vertical" aria-label="Lane header width"
                data-status-hint="Drag to resize lane headers" @pointerdown.stop="resizeTimelineHeaders" />
            <div class="node-column iv-graph-header" :class="{ 'inline-ports': laneContentHeight < editableHeightPx }"
                :style="{ '--graph-lane-height': `${laneContentHeight}px` }">
                <div ref="playbackReadout" class="playback-position" data-status-shortcuts="Space ▶/Ⅱ">
                    <template v-if="playbackSecondsLabel !== null">
                        <span class="playback-seconds">
                            {{ playbackSymbol }} {{ playbackSecondsLabel }} s
                        </span>
                        <small class="iv-graph-header-detail">{{ playback.backgroundPlayhead.value?.toString() }} smp</small>
                    </template>
                </div>
                <div v-if="gridMode === 'custom'" class="timeline-snap-input">
                    <NodePorts v-if="customInput?.ports.length" :node="customInput" :inline="false" />
                    <span v-else class="iv-graph-header-detail" role="status">Loading Beats input…</span>
                </div>
            </div>
            <div ref="timeRegion" class="time-region">
                <TimeRuler :window="timeWindow" :marks="timelineMarks" :beat-strengths="beatStrengths" :beat-labels="beatLabels"
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
        <div class="lanes" :style="{ '--nested-rim-height': `${Math.min(9, laneHeight / 2)}px` }">
            <div class="header-width-handle" aria-hidden="true" data-status-hint="Drag to resize lane headers"
                @pointerdown.stop="resizeTimelineHeaders" />
            <div v-for="area in nestedAreas" :key="area.key" class="nested-lane-area"
                :style="{ top: `${area.begin * laneHeight}px`, height: `${(area.end - area.begin) * laneHeight}px` }"
                aria-hidden="true" />
            <template v-if="gridMode !== 'seconds'">
                <svg v-for="run in timelineRuns" :key="run.begin" class="lane-timeline-overlay lane-snap-grid" aria-hidden="true"
                    :style="run.style">
                    <GridGuides :marks="timelineMarks" :strengths="beatStrengths"
                        :mark-key="timelineMarkKey" :position="timelineMarkPosition" />
                </svg>
            </template>
            <section v-for="{ child, depth, rowKey, semantic, moduleBoundary } in children" :key="rowKey"
                class="lane" :class="{ selected: !moduleBoundary && selectedLanes.has(addressKey(child.address)) }"
                :data-lane-row="rowKey"
                :data-lane-address="addressKey(child.address)"
                :data-lane-key="moduleBoundary ? undefined : addressKey(child.address)"
                :style="{ height: `${laneHeight}px` }" tabindex="-1"
                :data-status-hint="depth ? `Module nesting depth ${depth}` : undefined"
                :aria-label="(moduleBoundary ? `${nodeDisplayName(semantic)} module ports`
                    : formatLaneLabel(laneLabels(nodeDisplayName(semantic), semantic.sourceLabel))
                        + (selectedLanes.has(addressKey(child.address)) ? ', selected' : ''))
                    + (depth ? `, module nesting depth ${depth}` : '')">
                <PresentationHost v-if="moduleBoundary" :node="semantic" :region-height="laneContentHeight" module-boundary />
                <PresentationHost v-else :node="semantic" :region-height="laneContentHeight" lane
                    :open-source="child.virtualNode && child.declarationSource
                        ? () => openVirtualNodeSource(child.address) : undefined"
                    :rename-lane="scope.editable && isProjectInstance(child.address)
                        ? (name, previousName) => renameLane(child.instance.instanceId, name, previousName) : undefined"
                    @copy-debug-details="copyNodeDebugDetails(child.address)" />
            </section>
            <div v-for="{ key, begin } in nestedAreas" :key="key" class="nested-lane-rim start"
                :style="{ top: `${begin * laneHeight}px` }" aria-hidden="true" />
            <div v-for="end in nestedAreaEnds" :key="end" class="nested-lane-rim end"
                :style="{ top: `${end * laneHeight}px` }" aria-hidden="true" />
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
.timeline-scope { height: 100%; min-height: 0; display: flex; flex-direction: column; overflow-y: auto;
    scrollbar-gutter: stable; scrollbar-color: auto; scrollbar-width: auto;
    --node-label-width: clamp(var(--minimum-header-width), var(--preferred-header-width, clamp(112px, 17%, 182px)),
        var(--maximum-header-width)); }
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
.header-width-handle { position: absolute; inset-block: 0; inset-inline-start: calc(var(--node-label-width) - 4px);
    width: 8px; z-index: 2; cursor: col-resize; touch-action: none; }
.header-width-handle[data-hovered], .header-width-handle:active {
    background: linear-gradient(to right, transparent 3px, var(--vscode-focusBorder) 3px 5px, transparent 5px);
}
.node-column { display: grid; grid-template-rows: auto minmax(0, 1fr); min-width: 0; min-height: 0;
    border-right: 1px solid var(--vscode-panel-border); padding-inline: var(--iv-graph-header-inset); }
.timeline-snap-input { min-height: 0; }
.timeline-snap-input :deep(.node-ports) { height: 100%; margin: 0; }
.inline-ports .timeline-snap-input :deep(.port-group[data-direction="input"]) {
    /* Center the ruler socket where a single symbol sits in the compact input column. */
    --port-control-column: calc(100cqw - var(--inline-port-width) + var(--inline-stack-spacing) / 2);
    --port-socket-size: var(--inline-symbol-size);
    --socket-tuck: 0px;
}
.lanes { flex: 0 0 auto; min-height: 0; position: relative; isolation: isolate; }
/* Inner groups paint over outer groups and use the same left edge. */
.nested-lane-area {
    position: absolute; inset-inline: 0; z-index: -3; pointer-events: none;
    box-shadow: inset 6px 0 9px -6px var(--vscode-scrollbar-shadow, #0009);
    border-start-start-radius: 3px; border-end-start-radius: 3px;
    border-inline-start: 1px solid var(--vscode-panel-border);
    background: color-mix(in srgb, var(--vscode-editor-background) 97%, black);
}
/* Paint the horizontal edges above the lane backgrounds. */
.nested-lane-rim { position: absolute; inset-inline: 0; height: var(--nested-rim-height); z-index: 0; pointer-events: none; }
.nested-lane-rim.start { box-shadow: inset 0 6px 9px -5px var(--vscode-scrollbar-shadow, #0009); }
.nested-lane-rim.end {
    transform: translateY(-100%);
    box-shadow: inset 0 -5px 8px -5px var(--vscode-scrollbar-shadow, #0009);
}
.lane-timeline-overlay { position: absolute; inset-inline-start: var(--node-label-width);
    width: calc(100% - var(--node-label-width)); pointer-events: none; }
.lane-snap-grid { z-index: -1; }
.playback-position { display: flex; align-items: baseline; gap: 6px; box-sizing: border-box;
    height: 32px; padding-top: 8px; overflow: hidden;
    white-space: nowrap; font-variant-numeric: tabular-nums; color: var(--iv-lane-title-color); }
.playback-seconds { flex: 0 0 auto; }
.playback-position small { min-width: 0; overflow: hidden; text-overflow: ellipsis; color: var(--iv-port-label-base); }
.lane { position: relative; box-sizing: border-box; border-bottom: var(--lane-border-width) solid transparent; }
.lane[data-module-footer-target] { cursor: pointer; }
.lane[data-lane-drop="inside"] { box-shadow: inset 0 0 0 1px var(--vscode-focusBorder); }
.lane[data-lane-drop="before"] { box-shadow: inset 0 2px var(--vscode-focusBorder); }
.lane[data-lane-drop="after"] { box-shadow: inset 0 -2px var(--vscode-focusBorder); }
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

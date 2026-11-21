<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, useId, watch } from "vue";
import { automationTensionLimits, automationWindow, type CurveAxes, type CurveDragKind } from "../../src/automationCurveEditing";
import { automationSampleOffset, automationValueFraction, automationValueGuides, clampAutomationValue,
    compareAutomationTime, panAutomationValueAxis, panCurvedAutomationValueZoom, rankAutomationValueGuides, readAutomationCurveView,
    zoomAutomationValueAxis, type AutomationPoint, type AutomationPointPosition, type AutomationValueAxis, type AutomationValueDisplay,
    type AutomationCurveView, type AutomationValueRuler, type AutomationValueZoom, type CurveInterpolation } from "../../src/automationCurveGeometry";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { graphEditorShortcut, isTextEditingTarget } from "../../src/graphEditorShortcuts";
import { gridGuideStrengths } from "../../src/gridGuideStrength";
import { automationBoundTracksForSpans,
    sampleAutomationPreview, automationBoundVisiblePositions, type AutomationBoundInput,
    type BoundSample } from "../../src/automationBoundInputs";
import { sampleWindow } from "../../src/sampleWaveform";
import { usePortObservationViewport, usePresentationActive } from "./observationContext";
import GridGuides from "../timeline/GridGuides.vue";
import { usePresentationSize } from "./presentationSize";
import { rightClickHint } from "../../src/statusBarText";
import { useOptionalGraphEditor } from "../graph/context";
import { automationPointStatus } from "../statusHints";
import { automationTimelineTicks, snapTimelineSample, timelineSnapRadiusPx } from "../timeline/geometry";
import { usePlayback } from "../timeline/playbackContext";
import { visibleCursorDestination } from "../graph/cursorReturn";
import RulerGraduations from "../timeline/RulerGraduations.vue";
import { useTimelineMarks, useTimelineNavigation, useTimelineWindow, type LaneValueNavigation } from "../timeline/timelineContext";
import NodeLane from "./NodeLane.vue";
import Knob from "./Knob.vue";
import LaneSettings from "./LaneSettings.vue";
import AnchoredPopover from "./AnchoredPopover.vue";
import ContextMenuItem from "./ContextMenuItem.vue";
import { isContextMenuKey, useContextPopover } from "./contextPopover";
import { useAutomationEditing } from "./automationEditingContext";
import { nanoHeightPx, type NodeDisplayMode } from "./displayMode";
import { useLane } from "./laneContext";
import { useNode } from "./nodeContext";
import { usePresentationState } from "./presentationStateContext";

const props = defineProps<{ mode: NodeDisplayMode }>();
const editable = computed(() => props.mode === "editable");
const laneContext = useLane();
const hideLabels = computed(() => props.mode === "nano"
    || (props.mode === "compact" && (laneContext?.regionHeight.value ?? Infinity) <= nanoHeightPx));
const node = useNode();
const editor = useOptionalGraphEditor();
const profileGraphUi = editor?.profileGraphUi ?? (<T>(_: string, run: () => T) => run());
const { edits, version } = useAutomationEditing();
const time = useTimelineWindow();
const timelineMarks = useTimelineMarks();
const { backgroundPlayhead } = usePlayback();
const showBeats = computed(() => time?.value.gridMode === "beats" || time?.value.gridMode === "custom");
const navigation = useTimelineNavigation();
const surface = ref<SVGSVGElement | null>(null);
const size = usePresentationSize(surface);
const width = computed(() => Math.max(1, size.value?.width ?? 1));
const height = computed(() => Math.max(1, size.value?.height ?? 1));
const key = computed(() => node.value.address ? addressKey(node.value.address) : null);
const drawing = usePresentationActive(() => {
    void version.value;
    const address = node.value.address;
    return !!marquee.value || segmentPoint.value !== null || (!!address && edits.axes(address) !== null);
});
const curve = computed(() => {
    void version.value;
    return node.value.address ? edits.view(node.value.address) : null;
});
function boundPort(side: AutomationBoundInput) {
    const port = node.value.ports.find((entry) => entry.name === side && entry.direction === "input");
    return port?.kind === "continuous" ? port : undefined;
}
function manualBound(side: AutomationBoundInput) {
    const port = boundPort(side);
    const manual = port?.manualControl;
    return port?.connectivity !== "connected" && manual
        ? manual.liveValue ?? (manual.participates ? manual.value : undefined) : undefined;
}
const manualBounds = computed(() => ({ min: manualBound("min"), max: manualBound("max") }));
const timelineWindow = computed<{ beginSeconds: number; endSeconds: number; spanSeconds: number } | null>((previous) => {
    if (!curve.value) return null;
    const timeline = time?.value;
    const first = curve.value.points[0]?.seconds ?? 0;
    const last = curve.value.points.at(-1)?.seconds ?? first;
    const beginSeconds = timeline?.beginSeconds ?? (last > first ? first : first - .5);
    const endSeconds = timeline?.endSeconds ?? (last > first ? last : first + .5);
    const spanSeconds = timeline?.spanSeconds ?? (endSeconds - beginSeconds);
    if (previous?.beginSeconds === beginSeconds && previous.endSeconds === endSeconds && previous.spanSeconds === spanSeconds)
        return previous;
    return { beginSeconds, endSeconds, spanSeconds };
});
const observationWindow = computed(() => {
    const timeline = timelineWindow.value;
    return timeline && sampleWindow(timeline.beginSeconds, timeline.endSeconds,
        curve.value?.sampleRate, automationBoundVisiblePositions)
        || undefined;
});
function observeBound(side: AutomationBoundInput) {
    const observation = usePortObservationViewport(() => {
        const address = node.value.address, window = observationWindow.value;
        if (address && window && boundPort(side)?.connectivity === "connected") return {
            source: { node: address, kind: "input", member: side }, operation: "waveform",
            window, completeOnly: true,
        };
    });
    return computed(() => profileGraphUi("Refine automation bound tracks", () => {
        const timeline = timelineWindow.value, rate = curve.value?.sampleRate;
        const window = observationWindow.value, view = observation.value;
        return timeline && rate && window && view ? automationBoundTracksForSpans(view.spans, window,
            timeline.beginSeconds, timeline.spanSeconds, rate) : [];
    }));
}
const lowerBound = observeBound("min"), upperBound = observeBound("max");
const view = computed(() => {
    if (!drawing.value) return null;
    void version.value;
    const timeline = timelineWindow.value, address = node.value.address;
    return curve.value && timeline && address
        ? automationWindow(curve.value, 0, timeline.spanSeconds, edits.drafts(address), timeline.beginSeconds) : null;
});
const preview = computed(() => drawing.value ? profileGraphUi("Sample automation preview", () => sampleAutomationPreview(
    view.value?.paths ?? [], lowerBound.value, upperBound.value, manualBounds.value,
    timelineWindow.value?.spanSeconds ?? 0, width.value)) : undefined);
const curveView = ref<AutomationCurveView>({});
const viewChanged = usePresentationState({
    serialize() {
        const state = readAutomationCurveView(curveView.value);
        return Object.keys(state).length ? state : null;
    },
    restore(state) { curveView.value = readAutomationCurveView(state); },
});
const ruler = computed(() => curveView.value.ruler ?? "auto");
const display = computed(() => curveView.value.display ?? "curved");
const valuePan = computed(() => curveView.value.pan ?? 0);
const valueZoom = computed(() => curveView.value.zoom);
const axes = computed(() => {
    const window = timelineWindow.value;
    if (!window || !node.value.address) return null;
    const fixed = edits.axes(node.value.address);
    return { begin: window.beginSeconds, end: window.endSeconds, spanSeconds: window.spanSeconds,
        pan: fixed ? fixed.pan : valuePan.value, zoom: fixed ? fixed.zoom : valueZoom.value,
        display: fixed?.display ?? display.value, ruler: fixed ? fixed.ruler : ruler.value };
});
const ticks = computed(() => drawing.value && axes.value ? automationTimelineTicks({
    beginSeconds: axes.value.begin, endSeconds: axes.value.end, spanSeconds: axes.value.spanSeconds,
}, width.value, curve.value?.sampleRate) : []);
const x = (offset: number) => axes.value ? offset / axes.value.spanSeconds * width.value : 0;
const sampleX = (index: bigint) => curve.value?.sampleRate && axes.value
    ? x(automationSampleOffset(index, axes.value.begin, curve.value.sampleRate)) : undefined;
const pointX = (point: AutomationPointPosition) => (point.sampleIndex === undefined ? undefined : sampleX(point.sampleIndex))
    ?? x(point.seconds - (axes.value?.begin ?? 0));
const y = (value: number) => (1 - automationValueFraction(value, axes.value ?? {})) * height.value;
function previewPath(tracks?: BoundSample[][]): string {
    if (!tracks?.length) return "";
    return profileGraphUi("Build automation SVG paths", () => tracks.map((track) => track.map(({ seconds, value }, index) =>
        `${index ? "L" : "M"}${x(seconds)},${y(value)}`).join(" ")).join(" "));
}
const hasOutputBounds = computed(() => !!(preview.value?.min.length || preview.value?.max.length));
const minPath = computed(() => previewPath(preview.value?.min));
const maxPath = computed(() => previewPath(preview.value?.max));
const path = computed(() => previewPath(preview.value?.authored));
const outputPath = computed(() => hasOutputBounds.value ? previewPath(preview.value?.output) : "");
const markerY = (point: AutomationPoint) => Math.max(2, Math.min(height.value - 2, y(point.value)));
const valueGuides = computed(() => drawing.value && axes.value
    ? rankAutomationValueGuides(automationValueGuides(axes.value, height.value, axes.value.ruler)) : []);
const valueGuideKey = ({ value }: { value: number }) => value;
const valueGuidePosition = ({ y }: { y: number }) => y;
const valueStrengths = computed(() => gridGuideStrengths(valueGuides.value, valueGuidePosition));
const valueLabelInset = computed(() => hideLabels.value || !valueGuides.value.length ? 0
    : 9 + 6 * Math.max(...valueGuides.value.map(({ value }) => String(value).length)));
const valueLabelY = (guideY: number) => guideY >= 12 ? guideY - 3 : Math.min(height.value - 1, guideY + 10);
const snapGradientId = useId();
const snapFadeLength = 144;
const snapGuide = computed(() => {
    void version.value;
    const address = node.value.address;
    const snap = editable.value && address ? edits.activeSnap(address) : null;
    if (!snap) return null;
    const guideY = y(snap.value);
    if (!(guideY >= 0 && guideY <= height.value)) return null;
    return { value: snap.value, y: guideY, x: Math.max(0, Math.min(width.value, pointX(snap.point))) };
});
const snapLines = computed(() => {
    void version.value;
    const lines: Array<{ axis: "value" | "time"; start: number; end: number; transform: string }> = [];
    const value = snapGuide.value;
    if (value) lines.push({ axis: "value", start: -value.x, end: width.value - value.x,
        transform: `translate(${value.x} ${value.y})` });
    const address = node.value.address;
    const time = editable.value && address ? edits.activeTimeSnap(address) : null;
    const timeX = time ? sampleX(time.sampleIndex) : undefined;
    if (time && timeX !== undefined && timeX >= 0 && timeX <= width.value) {
        const pointY = Math.max(0, Math.min(height.value, y(time.point.value)));
        lines.push({ axis: "time", start: -pointY, end: height.value - pointY,
            transform: `translate(${timeX} ${pointY}) rotate(90)` });
    }
    return lines;
});

const markers = computed(() => editable.value ? (view.value?.points ?? []).filter((point) => {
    if (axes.value?.display !== "cropped" && !axes.value?.zoom) return true;
    const position = y(point.value);
    return position >= 0 && position <= height.value;
}) : []);
const selectedIds = computed(() => {
    void version.value;
    return node.value.address ? edits.selectedPoints(node.value.address) : new Set<number>();
});
const handles = computed(() => markers.value.filter((point, index, points) => point.interpolation === "bezier"
    && (!points[index + 1] || compareAutomationTime(point, points[index + 1]) !== 0)));
const handleY = (point: AutomationPoint) => {
    const center = markerY(point);
    return center + (center < height.value / 2 ? 15 : -15);
};

const { panel: popover, key: segmentPoint, open: openContext, close: closeMenus, toggled: menuToggled } = useContextPopover<number>();
const selected = computed(() => {
    void version.value;
    const point = curve.value?.points.find(({ id }) => id === segmentPoint.value);
    const target = node.value.address;
    if (!point || !target) return null;
    const draft = edits.drafts(target).find(({ id }) => id === point.id);
    return draft ? { ...point, ...draft } : point;
});
const marquee = ref<{ pointerId: number; x: number; y: number; currentX: number; currentY: number;
    ctrlKey: boolean; metaKey: boolean; shiftKey: boolean } | null>(null);
const selectionRect = computed(() => {
    const drag = marquee.value;
    return drag ? { x: Math.min(drag.x, drag.currentX), y: Math.min(drag.y, drag.currentY),
        width: Math.abs(drag.x - drag.currentX), height: Math.abs(drag.y - drag.currentY) } : null;
});

function editSegment(event: MouseEvent | KeyboardEvent, point: AutomationPoint) {
    if (!editable.value) return;
    const control = event.currentTarget as SVGElement;
    openContext(event, point.id, { anchor: control, alignRight: true,
        returnFocus: event instanceof KeyboardEvent ? control : surface.value, initialFocus: "select" });
}
function changeSegment(point: AutomationPoint, interpolation: CurveInterpolation, tension: number) {
    if (!editable.value || !node.value.address || !Number.isFinite(tension) || tension < 0 || tension > 1) return;
    void edits.segment(node.value.address, point.id, interpolation, Math.fround(tension));
}
function escapeSegment() {
    if (edits.activePoint()?.id === segmentPoint.value) edits.abandon();
}
watch(editable, (enabled) => {
    if (enabled) return;
    const pointerId = marquee.value?.pointerId;
    marquee.value = null;
    if (pointerId !== undefined && surface.value?.hasPointerCapture(pointerId)) surface.value.releasePointerCapture(pointerId);
    closeMenus();
});
watch(selected, (point) => { if (!point) closeMenus(); });

function changeDisplay(event: Event) {
    if (!editor || key.value === null) return;
    const mode = (event.target as HTMLSelectElement).value as AutomationValueDisplay;
    updateCurveView({ display: mode, pan: undefined, zoom: undefined }, true);
}
function changeRuler(event: Event) {
    if (!editor || key.value === null) return;
    const value = (event.target as HTMLSelectElement).value as AutomationValueRuler;
    updateCurveView({ ruler: value }, true);
}
const error = computed(() => {
    void version.value;
    return node.value.address ? edits.error(node.value.address) || node.value.constructionError : "";
});
const curveStatus = computed(() => error.value || (editable.value && curve.value?.sampleRate ? "" : "Read-only curve"));
const curveShortcuts = `${rightClickHint} Drag Add · Drag Select`;
const pointShortcuts = `⇧Drag Keep type · ⇧Click Delete · ${rightClickHint} Segment`;
function pointStatus(point: AutomationPoint) {
    const { id, sampleIndex, seconds, value } = point;
    return automationPointStatus({ id, sampleIndex, seconds, value });
}
function pointerAxes(): CurveAxes | null {
    if (!surface.value || !axes.value) return null;
    const rect = surface.value.getBoundingClientRect();
    const marks = showBeats.value ? (timelineMarks?.value ?? []).map(({ index }) => index) : [];
    const playhead = backgroundPlayhead.value;
    const tolerance = timelineSnapRadiusPx * (curve.value?.sampleRate ?? 0) * (axes.value.spanSeconds ?? axes.value.end - axes.value.begin)
        / Math.max(1, rect.width);
    return { ...axes.value, left: rect.left, top: rect.top, width: rect.width, height: rect.height,
        timeDetents: playhead === null ? marks : [...marks, playhead], snapTime: (index) => snapTimelineSample(index, marks, tolerance) };
}
function pointCursorPosition(control: SVGElement | null, target: NodeAddress, id: number) {
    const svg = surface.value;
    if (!svg || !editable.value) return null;
    if (control?.isConnected) {
        const rect = control.getBoundingClientRect();
        const x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
        if (document.elementFromPoint(x, y)?.closest(".curve-point") === control) return { x, y };
    }
    // A point can leave the visible list and get a new marker when it returns.
    // Use its current position when the element from the press is gone.
    const point = edits.drafts(target).find((point) => point.id === id) ?? curve.value?.points.find((point) => point.id === id);
    if (!point || !axes.value) return null;
    const rect = svg.getBoundingClientRect();
    return visibleCursorDestination(svg, { x: rect.left + pointX(point) * rect.width / width.value,
        y: rect.top + y(point.value) * rect.height / height.value });
}
function start(event: PointerEvent, id: number, kind: CurveDragKind = "point") {
    if (!editable.value || event.button !== 0) return;
    const axes = pointerAxes();
    const target = node.value.address;
    if (axes && target) {
        const control = event.currentTarget as SVGElement;
        event.preventDefault();
        surface.value?.focus({ preventScroll: true });
        editor?.startAutomation(event, target, id, axes, kind,
            kind === "point" ? (pointId) => pointCursorPosition(control, target, pointId) : undefined);
    }
}
function pressSurface(event: PointerEvent) {
    if (event.button !== 2) { beginSelection(event); return; }
    if (!editable.value) return;
    const address = node.value.address;
    const axes = pointerAxes();
    if (!address || !axes || !curve.value?.sampleRate) return;
    surface.value?.focus({ preventScroll: true });
    editor?.startAutomation(event, address, null, axes, "point", (id) => pointCursorPosition(null, address, id));
}
function selectionPosition(event: PointerEvent) {
    const rect = surface.value!.getBoundingClientRect();
    return { x: Math.max(0, Math.min(width.value, (event.clientX - rect.left) / rect.width * width.value)),
        y: Math.max(0, Math.min(height.value, (event.clientY - rect.top) / rect.height * height.value)) };
}
function beginSelection(event: PointerEvent) {
    if (!editable.value || !surface.value || event.button !== 0 || edits.pointerId() !== null) return;
    event.preventDefault();
    const { x, y } = selectionPosition(event);
    marquee.value = { pointerId: event.pointerId, x, y, currentX: x, currentY: y,
        ctrlKey: event.ctrlKey, metaKey: event.metaKey, shiftKey: event.shiftKey };
    surface.value.focus({ preventScroll: true });
    surface.value.setPointerCapture(event.pointerId);
}
function moveSelection(event: PointerEvent) {
    const drag = marquee.value;
    if (!drag || drag.pointerId !== event.pointerId) return;
    const { x, y } = selectionPosition(event);
    drag.currentX = x;
    drag.currentY = y;
}
function finishSelection(event: PointerEvent) {
    const drag = marquee.value;
    if (!drag || drag.pointerId !== event.pointerId) return;
    moveSelection(event);
    const rect = selectionRect.value!;
    marquee.value = null;
    if (!node.value.address) return;
    const ids = Math.hypot(rect.width, rect.height) < 3 ? [] : markers.value.filter((point) => {
        const x = pointX(point), y = markerY(point);
        return x >= rect.x && x <= rect.x + rect.width && y >= rect.y && y <= rect.y + rect.height;
    }).map((point) => point.id);
    edits.selectPoints(node.value.address, ids, drag);
}
function cancelSelection(event: PointerEvent) {
    if (marquee.value?.pointerId === event.pointerId) marquee.value = null;
}
function deleteSelected(event: KeyboardEvent) {
    event.preventDefault();
    event.stopPropagation();
    if (editable.value && node.value.address && !event.repeat) void edits.removeSelection(node.value.address);
}
function selectAllPoints(event: KeyboardEvent) {
    if (graphEditorShortcut(event) !== "selectAll" || isTextEditingTarget(event.target)) return;
    event.preventDefault();
    event.stopPropagation();
    const address = node.value.address;
    if (!event.repeat && editable.value && address && curve.value) {
        const points = [...curve.value.points, ...edits.drafts(address)];
        edits.selectPoints(address, points.map(({ id }) => id));
    }
}
function removePoint(id: number) {
    if (!editable.value || !node.value.address) return;
    if (segmentPoint.value === id) closeMenus(true);
    void edits.remove(node.value.address, id);
}
function pointKey(event: KeyboardEvent, point: AutomationPoint) {
    if (event.key === "Delete") {
        if (selectedIds.value.has(point.id)) deleteSelected(event);
        else {
            event.preventDefault();
            event.stopPropagation();
            if (!event.repeat) removePoint(point.id);
        }
    } else if (event.key === "Enter" || isContextMenuKey(event))
        editSegment(event, point);
}
function tensionBinding(point: AutomationPoint) {
    const target = node.value.address;
    return {
        beginEdit(event: PointerEvent) {
            const axes = pointerAxes();
            return !!target && !!axes && edits.begin(target, point.id, event.pointerId, axes,
                { x: event.clientX, y: event.clientY }, "tension");
        },
        endEdit() { void edits.finish(); },
        editActive: () => edits.pointerId() !== null,
        apply(value: number, live: boolean) {
            if (live && target) edits.setDraggedTension(target, point.id, value);
            else changeSegment(point, "bezier", value);
        },
    };
}

function adjustTension(event: KeyboardEvent, point: AutomationPoint): boolean {
    const adjustment = { ArrowLeft: -.05, ArrowDown: -.05, ArrowRight: .05, ArrowUp: .05 }[event.key];
    const value = event.key === "Home" ? 0 : event.key === "End" ? 1
        : adjustment === undefined ? undefined : clampAutomationValue(point.tension + adjustment, automationTensionLimits);
    if (value === undefined) return false;
    event.preventDefault();
    event.stopPropagation();
    if (value !== point.tension) changeSegment(point, "bezier", value);
    return true;
}
function tensionKey(event: KeyboardEvent, point: AutomationPoint) {
    if (!adjustTension(event, point)) pointKey(event, point);
}

let unregisterValueNavigation: (() => void) | undefined;
function updateCurveView(change: AutomationCurveView, saveNow = false) {
    curveView.value = { ...curveView.value, ...change };
    viewChanged(saveNow);
}
function setValueView(pan: number, zoom?: AutomationValueZoom) {
    updateCurveView({ pan, zoom: zoom?.factor === 1 ? undefined : zoom });
}
function valueNavigationTarget() {
    const axis = axes.value, element = surface.value;
    if (!editor || !axis || !element || edits.pointerId() !== null || marquee.value) return;
    return { axis, element };
}
function zoomValueAt(axis: AutomationValueAxis, element: SVGSVGElement, factor: number, cursorY: number) {
    const rect = element.getBoundingClientRect();
    if (rect.height > 0) return zoomAutomationValueAxis(axis, factor, 1 - (cursorY - rect.top) / rect.height);
}
const valueNavigation: LaneValueNavigation = {
    begin(mode) {
        const target = valueNavigationTarget();
        if (!target) return null;
        const { axis, element } = target;
        const initialPan = valuePan.value;
        const initialZoom = valueZoom.value ?? { factor: 1, center: .5 };
        let pan = initialPan, zoom = initialZoom;
        const laneHeight = height.value;
        const gesture = {
            content: element.closest(".lane-content") ?? element,
            changed() { return pan !== initialPan || zoom.factor !== initialZoom.factor || zoom.center !== initialZoom.center; },
        };
        function update(nextPan: number, nextZoom: AutomationValueZoom) {
            if (nextPan === pan && nextZoom.factor === zoom.factor && nextZoom.center === zoom.center) return;
            pan = nextPan;
            zoom = nextZoom;
            setValueView(pan, zoom);
        }
        if (mode === "zoom") return { ...gesture, mode, zoom(factor, cursorY) {
            const next = zoomValueAt({ ...axis, pan, zoom }, element, factor, cursorY);
            if (next) update(next.pan, next.zoom);
        } };
        return { ...gesture, mode, pan(deltaY) {
            if (axis.display !== "cropped" && zoom.factor > 1)
                update(pan, panCurvedAutomationValueZoom(zoom, deltaY, laneHeight));
            else update(panAutomationValueAxis({ ...axis, pan, zoom }, deltaY, laneHeight), zoom);
        } };
    },
    zoom(factor, cursorY) {
        const target = valueNavigationTarget();
        if (!target) return false;
        const { axis, element } = target;
        const next = zoomValueAt(axis, element, factor, cursorY);
        if (!next || next.zoom.factor === (axis.zoom?.factor ?? 1)) return false;
        setValueView(next.pan, next.zoom);
        return true;
    },
    reset() {
        if (editor && key.value !== null && (valuePan.value || valueZoom.value) && edits.pointerId() === null) {
            updateCurveView({ pan: undefined, zoom: undefined }, true);
        }
    },
};
onMounted(() => {
    const lane = surface.value?.closest(".lane");
    if (lane) unregisterValueNavigation = navigation?.registerValueNavigation(lane, valueNavigation);
});
onBeforeUnmount(() => {
    unregisterValueNavigation?.();
});
</script>

<template>
    <NodeLane :mode="mode">
        <div class="automation-surface">
            <RulerGraduations v-if="!showBeats" class="clip-ruler" :ticks="ticks" :label-inset="valueLabelInset" :hide-labels="hideLabels" compact />
            <svg ref="surface" class="automation-curve" :viewBox="`0 0 ${width} ${height}`" preserveAspectRatio="none"
                :aria-label="node.displayName || 'Automation curve'" :aria-description="curveStatus || curveShortcuts"
                :data-status-hint="curveStatus" :data-status-shortcuts="curveStatus ? undefined : curveShortcuts"
                :tabindex="editable ? -1 : undefined" :data-lane-item-editor="editable ? '' : undefined"
                @pointerdown.self="pressSurface" @pointermove="moveSelection" @pointerup="finishSelection"
                @pointercancel="cancelSelection" @lostpointercapture.self="cancelSelection"
                @keydown="selectAllPoints" @keydown.delete.self="deleteSelected" @contextmenu.self.prevent>
                <g class="value-guides" aria-hidden="true">
                    <GridGuides :marks="valueGuides" :strengths="valueStrengths" :mark-key="valueGuideKey"
                        :position="valueGuidePosition" axis="y" :end="width" />
                    <template v-if="!hideLabels">
                        <template v-for="guide in valueGuides" :key="guide.value">
                            <text v-if="guide.value !== snapGuide?.value" x="4" :y="valueLabelY(guide.y)">{{ guide.value }}</text>
                        </template>
                    </template>
                </g>
                <path :d="path" class="curve-line" :class="{ 'authored-curve': hasOutputBounds }" />
                <path v-if="outputPath" :d="outputPath" class="curve-line" />
                <g class="automation-bound-traces" aria-hidden="true">
                    <path v-if="minPath" :d="minPath" />
                    <path v-if="maxPath" :d="maxPath" class="upper-bound" />
                </g>
                <g v-if="snapLines.length" class="snap-guides" aria-hidden="true">
                    <defs>
                        <linearGradient :id="snapGradientId" gradientUnits="userSpaceOnUse"
                            :x1="-snapFadeLength" :x2="snapFadeLength" y1="0" y2="0">
                            <stop offset="0" stop-opacity="0" />
                            <stop offset=".25" stop-opacity=".2" />
                            <stop offset=".5" stop-opacity=".95" />
                            <stop offset=".75" stop-opacity=".2" />
                            <stop offset="1" stop-opacity="0" />
                        </linearGradient>
                    </defs>
                    <line v-if="snapGuide" x1="0" :x2="width" :y1="snapGuide.y" :y2="snapGuide.y"
                        stroke="currentColor" stroke-opacity=".35" vector-effect="non-scaling-stroke" />
                    <line v-for="guide in snapLines" :key="guide.axis" :x1="guide.start" :x2="guide.end" y1="0" y2="0" :transform="guide.transform"
                        :stroke="`url(#${snapGradientId})`" stroke-width="1.25" vector-effect="non-scaling-stroke" />
                    <text v-if="snapGuide && !hideLabels" x="4" :y="valueLabelY(snapGuide.y)">{{ snapGuide.value }}</text>
                </g>
                <rect v-if="selectionRect" v-bind="selectionRect" class="point-selection" aria-hidden="true" />
                <g v-for="point in handles" :key="`tension-${point.id}`" class="curve-tension"
                    :transform="`translate(${pointX(point)} ${handleY(point)})`"
                    role="slider" tabindex="-1" aria-label="Bézier tension" :aria-valuemin="0" :aria-valuemax="1"
                    :aria-valuenow="point.tension" :data-status-hint="`Tension ${Number(point.tension.toFixed(2))}`" data-status-shortcuts="↔ Drag"
                    @pointerdown="start($event, point.id, 'tension')" @click.stop
                    @keydown="tensionKey($event, point)" @contextmenu.stop="editSegment($event, point)">
                    <path class="tension-stem" :d="`M0 0 V${markerY(point) - handleY(point)}`" />
                    <circle class="control-target" r="6" />
                    <path class="tension-marker" d="M0 -3.4 L3.4 0 L0 3.4 L-3.4 0Z"
                        :fill-opacity=".25 + point.tension * .75" />
                </g>
                <g v-for="point in markers" :key="point.id" class="curve-point"
                    :transform="`translate(${pointX(point)} ${markerY(point)})`" role="button" tabindex="-1"
                    :data-selected="selectedIds.has(point.id) || undefined"
                    :aria-label="`Point at ${point.sampleIndex ?? point.seconds} ${point.sampleIndex === undefined ? 'seconds' : 'samples'}, value ${point.value}${selectedIds.has(point.id) ? ', selected' : ''}`"
                    :data-status-hint="pointStatus(point)" :data-status-shortcuts="pointShortcuts"
                    aria-description="Drag to place. Hold Shift to keep the point type. Shift-click to delete. Right-click for segment options."
                    @pointerdown="start($event, point.id)"
                    @click.stop
                    @keydown="pointKey($event, point)" @contextmenu.stop="editSegment($event, point)">
                    <circle class="control-target" r="8" />
                    <circle v-if="selectedIds.has(point.id)" class="selected-point-ring" r="5.5" />
                    <circle class="point-marker" r="2.6" />
                </g>
            </svg>
            <LaneSettings v-if="editable && editor && curve" label="Scale" dialog-label="Automation value scale"
                hint="Set value display and snapping" :width="204" :preferred-height="158" @toggle="(open) => open && closeMenus()">
                <label class="automation-settings-row">Display <select tabindex="-1" :value="display" @change="changeDisplay">
                    <option value="curved">Curved</option><option value="cropped">Cropped</option>
                </select></label>
                <label class="automation-settings-row">Value snapping
                    <select tabindex="-1" :value="ruler" data-status-hint="Auto adds finer guides as you zoom in" @change="changeRuler">
                        <option value="auto">Auto</option><option value="whole">Whole</option>
                        <option value="half">Halves</option><option value="off">Off</option>
                    </select>
                </label>
                <span v-if="error" role="alert" class="scale-error">{{ error }}</span>
            </LaneSettings>
        </div>
    </NodeLane>
    <Teleport to="body">
        <AnchoredPopover v-if="editable && segmentPoint !== null" ref="popover" class="automation-popover" label="Automation segment"
            :data-lane-key="key" data-lane-item-editor contextual @escape="escapeSegment" @toggle="menuToggled" @keydown="selectAllPoints">
            <template v-if="selected">
                <div class="automation-menu-fields">
                    <label>Segment <select tabindex="-1" :value="selected.interpolation"
                        @change="changeSegment(selected, ($event.target as HTMLSelectElement).value as CurveInterpolation, selected.tension)">
                        <option value="empty">Empty</option><option value="hold">Hold</option><option value="bezier">Bézier</option>
                    </select></label>
                    <div v-if="selected.interpolation === 'bezier'" class="automation-popover-control">
                        <span aria-hidden="true">Tension</span>
                        <Knob :identity="`${key}:automation:tension:${selected.id}`" label="Bézier tension"
                            :value="selected.tension" :limits="automationTensionLimits" :step=".05" v-bind="tensionBinding(selected)" />
                    </div>
                </div>
                <hr class="context-menu-separator" />
                <ContextMenuItem @click="removePoint(selected.id)">Delete point</ContextMenuItem>
            </template>
        </AnchoredPopover>
    </Teleport>
</template>

<style scoped>
.automation-surface { position: relative; width: 100%; height: 100%; min-height: 1px; color: var(--iv-port-input); }
.clip-ruler { --iv-lane-ruler-tick: color-mix(in srgb, var(--vscode-foreground) 30%, transparent); }
.automation-curve { position: relative; display: block; width: 100%; height: 100%; overflow: hidden; touch-action: none; }
.value-guides { pointer-events: none; }
:is(.value-guides, .snap-guides) text { font-size: 9px; font-variant-numeric: tabular-nums; }
.value-guides text { fill: var(--vscode-descriptionForeground); opacity: .75; }
.snap-guides { pointer-events: none; color: var(--vscode-charts-blue, #3794ff); }
.snap-guides stop { stop-color: currentColor; }
.snap-guides text { fill: currentColor; font-weight: 600; }
.automation-bound-traces { pointer-events: none; }
.automation-bound-traces path { fill: none; stroke: var(--vscode-charts-blue, #3794ff); stroke-width: 1; opacity: .55; stroke-dasharray: 3 3; }
.automation-bound-traces .upper-bound { stroke: var(--vscode-charts-orange, #d18616); }
.authored-curve { opacity: .32; }
.curve-line { fill: none; stroke: currentColor; stroke-width: 1.5; pointer-events: none; }
.point-selection { pointer-events: none; fill: color-mix(in srgb, var(--vscode-focusBorder) 13%, transparent);
    stroke: var(--vscode-focusBorder); stroke-width: 1; }
.automation-curve { user-select: none; -webkit-user-select: none; }
.curve-point { cursor: default; }
.curve-tension { cursor: ew-resize; }
.control-target { fill: transparent; }
.selected-point-ring {
    fill: var(--vscode-editor-background); stroke: var(--vscode-focusBorder);
    stroke-width: 1.5px; pointer-events: none;
}
.point-marker, .tension-marker { fill: currentColor; pointer-events: none; }
.curve-point[data-hovered] .point-marker { r: 3.3px; }
.curve-point[data-selected] .point-marker { r: 3.2px; }
.tension-stem { fill: none; stroke: currentColor; stroke-opacity: .22; pointer-events: none; }
.curve-tension[data-hovered] .tension-marker { fill-opacity: 1; transform: scale(1.27); }
.automation-popover { width: min(214px, calc(100vw - 8px)); }
.automation-menu-fields { display: grid; gap: 4px; padding: 4px 8px; }
.automation-popover label, .automation-popover-control, .automation-settings-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.automation-popover select, .automation-settings-row select {
    max-width: 100px; color: var(--vscode-input-foreground); background: var(--vscode-input-background);
    border: 1px solid var(--vscode-input-border); font: inherit;
}
.scale-error { color: var(--vscode-errorForeground); }
</style>

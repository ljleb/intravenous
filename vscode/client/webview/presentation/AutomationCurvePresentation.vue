<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { automationWindow, curvePointAt, type CurveAxes, type CurveDragKind } from "../../src/automationCurveEditing";
import { automationBoundLimits, automationDisplayBounds, automationSampleOffset, automationValueFraction, automationValueGuides, clampAutomationValue,
    compareAutomationTime, defaultAutomationBounds, enabledAutomationBounds, panAutomationValueAxis, type AutomationBoundSide, type AutomationPoint,
    type AutomationValueRuler, type CurveInterpolation, type CurvePath } from "../../src/automationCurveGeometry";
import { addressKey } from "../../src/graphScope";
import { knobScrubSpeed } from "../../src/knobPresentation";
import { rightClickHint } from "../../src/statusBarText";
import { useOptionalGraphEditor } from "../graph/context";
import { automationPointStatus } from "../statusHints";
import { automationTimelineTicks } from "../timeline/geometry";
import RulerGraduations from "../timeline/RulerGraduations.vue";
import TimelinePlayhead from "../timeline/TimelinePlayhead.vue";
import { useTimelineNavigation, useTimelineWindow, type LaneValueNavigation } from "../timeline/timelineContext";
import NodeLane from "./NodeLane.vue";
import KnobFace from "./KnobFace.vue";
import { useAutomationEditing } from "./automationEditingContext";
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";

const props = defineProps<{ mode: NodeDisplayMode }>();
const editable = computed(() => props.mode === "editable");
const node = useNode();
const editor = useOptionalGraphEditor();
const { edits, version } = useAutomationEditing();
const time = useTimelineWindow();
const navigation = useTimelineNavigation();
const surface = ref<SVGSVGElement | null>(null);
const width = ref(1), height = ref(1);
const key = computed(() => node.value.address ? addressKey(node.value.address) : null);
const curve = computed(() => {
    void version.value;
    return node.value.address ? edits.view(node.value.address) : null;
});
const authoredBounds = computed(() => {
    void version.value;
    return node.value.address ? edits.bounds(node.value.address) : undefined;
});
const boundsPending = computed(() => {
    void version.value;
    return !!node.value.address && edits.boundsPending(node.value.address);
});
const eventBounds = computed(() => enabledAutomationBounds(authoredBounds.value));
const bounds = computed(() => automationDisplayBounds(eventBounds.value));
const boundControls = computed(() => {
    const settings = authoredBounds.value ?? defaultAutomationBounds;
    return (["min", "max"] as const).map((side) => ({
        side, name: side === "min" ? "Minimum" : "Maximum", ...settings[side],
        limits: { ...automationBoundLimits(settings, side), defaultValue: defaultAutomationBounds[side].value },
    }));
});
const ruler = computed(() => (key.value === null ? undefined : editor?.layout.value.automationRulers?.[key.value]) ?? "auto");
const valuePan = computed(() => key.value === null ? 0 : editor?.layout.value.automationValuePans?.[key.value] ?? 0);
const axes = computed(() => {
    if (!curve.value || !node.value.address) return null;
    const { points } = curve.value;
    const first = points[0]?.seconds ?? 0;
    const last = points.at(-1)?.seconds ?? first;
    const window = time?.value ?? (last > first
        ? { beginSeconds: first, endSeconds: last }
        : { beginSeconds: first - .5, endSeconds: first + .5 });
    const fixed = edits.axes(node.value.address);
    const spanSeconds = time?.value.spanSeconds ?? window.endSeconds - window.beginSeconds;
    return { begin: window.beginSeconds, end: window.endSeconds, spanSeconds, min: fixed ? fixed.min : bounds.value.min,
        max: fixed ? fixed.max : bounds.value.max, pan: valuePan.value, ruler: fixed ? fixed.ruler : ruler.value };
});
const view = computed(() => {
    void version.value;
    return curve.value && axes.value && node.value.address
        ? automationWindow(curve.value, 0, axes.value.spanSeconds, edits.drafts(node.value.address), axes.value.begin, eventBounds.value) : null;
});
const ticks = computed(() => axes.value ? automationTimelineTicks({
    beginSeconds: axes.value.begin, endSeconds: axes.value.end, spanSeconds: axes.value.spanSeconds,
}, width.value, curve.value?.sampleRate) : []);
const x = (offset: number) => axes.value ? offset / axes.value.spanSeconds * width.value : 0;
const pointX = (point: AutomationPoint) => x(curve.value?.sampleRate && axes.value && point.sampleIndex !== undefined
    ? automationSampleOffset(point.sampleIndex, axes.value.begin, curve.value.sampleRate)
    : point.seconds - (axes.value?.begin ?? 0));
const y = (value: number) => (1 - automationValueFraction(value, axes.value ?? {})) * height.value;
const eventValue = (point: AutomationPoint) => clampAutomationValue(point.value, eventBounds.value);
const markerY = (point: AutomationPoint) => Math.max(2, Math.min(height.value - 2, y(eventValue(point))));
const valueGuides = computed(() => axes.value ? automationValueGuides(axes.value, height.value, axes.value.ruler) : []);
const valueLabelInset = computed(() => props.mode === "nano" || !valueGuides.value.length ? 0
    : 9 + 6 * Math.max(...valueGuides.value.map(({ value }) => String(value).length)));

function segmentPath(segment: CurvePath): string {
    const a = x(segment.begin.seconds), b = x(segment.end.seconds);
    const start = `M${a},${y(segment.begin.value)}`;
    if (axes.value?.min !== undefined && axes.value.max !== undefined) {
        const offset = (b - a) / 3;
        return `${start} C${a + offset},${y(segment.first)} ${b - offset},${y(segment.second)} ${b},${y(segment.end.value)}`;
    }
    // Evaluate in Sample values before applying the nonlinear display scale.
    const mapped = (t: number) => {
        const u = 1 - t;
        const value = u ** 3 * segment.begin.value + 3 * u ** 2 * t * segment.first
            + 3 * u * t ** 2 * segment.second + t ** 3 * segment.end.value;
        return y(value);
    };
    const points = [start];
    function append(from: number, to: number, fromY: number, toY: number, depth: number) {
        const span = to - from;
        const middle = from + span / 2;
        const middleY = mapped(middle);
        const rise = toY - fromY;
        if (depth < 12 && (Math.abs(middleY - (fromY + rise / 2)) > .35
            || Math.abs(mapped(from + span / 4) - (fromY + rise / 4)) > .35
            || Math.abs(mapped(from + 3 * span / 4) - (fromY + 3 * rise / 4)) > .35)) {
            append(from, middle, fromY, middleY, depth + 1);
            append(middle, to, middleY, toY, depth + 1);
        } else points.push(`L${a + (b - a) * to},${toY}`);
    }
    append(0, 1, y(segment.begin.value), y(segment.end.value), 0);
    return points.join(" ");
}
const path = computed(() => view.value?.paths.map(segmentPath).join(" ") ?? "");
const markers = computed(() => editable.value ? view.value?.points ?? [] : []);
const selectedId = computed(() => {
    void version.value;
    return node.value.address ? edits.selectedPoint(node.value.address) : undefined;
});
const handles = computed(() => markers.value.filter((point, index, points) => point.interpolation === "bezier"
    && (!points[index + 1] || compareAutomationTime(point, points[index + 1]) !== 0)));
const handleY = (point: AutomationPoint) => {
    const center = markerY(point);
    return center + (center < height.value / 2 ? 15 : -15);
};

type PopoverPosition = { left: number; top: number };
const segmentMenu = ref<({ id: number } & PopoverPosition) | null>(null);
const scaleMenu = ref<PopoverPosition | null>(null);
const popover = ref<HTMLElement | null>(null);
const scaleButton = ref<HTMLButtonElement | null>(null);
const selected = computed(() => curve.value?.points.find((point) => point.id === segmentMenu.value?.id) ?? null);
let menuAnchor: SVGElement | null = null;
let pointClick: { pointerId: number; x: number; y: number; pointId?: number } | null = null;

function selectPoint(id: number) {
    if (node.value.address) edits.select(node.value.address, id);
}
function menuPosition(x: number, y: number, width: number, height: number): PopoverPosition {
    return { left: Math.max(4, Math.min(window.innerWidth - width, x)),
        top: Math.max(4, Math.min(window.innerHeight - height, y)) };
}
function editSegment(event: MouseEvent | KeyboardEvent, point: AutomationPoint) {
    if (!editable.value) return;
    event.preventDefault();
    event.stopPropagation();
    selectPoint(point.id);
    menuAnchor = event.currentTarget as SVGElement;
    const rect = menuAnchor.getBoundingClientRect();
    const x = event instanceof MouseEvent ? event.clientX : rect.right;
    const y = event instanceof MouseEvent ? event.clientY : rect.bottom;
    scaleMenu.value = null;
    segmentMenu.value = { id: point.id, ...menuPosition(x, y, 220, 145) };
    void nextTick(() => popover.value?.querySelector("select")?.focus({ preventScroll: true }));
}
function changeSegment(point: AutomationPoint, interpolation: CurveInterpolation, tension: number) {
    if (!editable.value || !node.value.address || !Number.isFinite(tension) || tension < 0 || tension > 1) return;
    void edits.segment(node.value.address, point.id, interpolation, Math.fround(tension));
}
function closeMenus(restoreFocus = false) {
    if (restoreFocus) (scaleMenu.value ? scaleButton.value : menuAnchor)?.focus({ preventScroll: true });
    segmentMenu.value = null;
    scaleMenu.value = null;
    menuAnchor = null;
}
function outside(event: PointerEvent) {
    pointClick = null;
    if (!(event.target instanceof Element) || !event.target.closest(".automation-popover, .automation-scale-button")) closeMenus();
}
watch(editable, (enabled) => { if (!enabled) { pointClick = null; closeMenus(); } });
watch(selected, (point) => { if (!point) segmentMenu.value = null; });

function showScale() {
    if (scaleMenu.value) { closeMenus(); return; }
    if (!scaleButton.value) return;
    const rect = scaleButton.value.getBoundingClientRect();
    segmentMenu.value = null;
    scaleMenu.value = menuPosition(rect.right - 192, rect.bottom + 4, 204, 158);
    void nextTick(() => popover.value?.querySelector<HTMLButtonElement>("button:not(:disabled)")?.focus({ preventScroll: true }));
}
function startBound(event: PointerEvent, side: AutomationBoundSide) {
    if (node.value.address) editor?.startAutomationBound(event, node.value.address, side);
}
function toggleBound(side: AutomationBoundSide) {
    const bound = (authoredBounds.value ?? defaultAutomationBounds)[side];
    if (node.value.address) void edits.setBound(node.value.address, side, { ...bound, enabled: !bound.enabled });
}
function stepBound(side: AutomationBoundSide, direction: number) {
    const control = boundControls.value.find((entry) => entry.side === side);
    if (node.value.address && control?.enabled) void edits.setBound(node.value.address, side, {
        enabled: true, value: control.value + direction * 2 * knobScrubSpeed(control.limits),
    });
}
function escapeScale() {
    if (edits.activeBound()) edits.abandon();
    closeMenus(true);
}
function changeRuler(event: Event) {
    if (!editor || key.value === null) return;
    const value = (event.target as HTMLSelectElement).value as AutomationValueRuler;
    const automationRulers = { ...editor.layout.value.automationRulers };
    if (value === "auto") delete automationRulers[key.value];
    else automationRulers[key.value] = value;
    editor.layout.value = { ...editor.layout.value, automationRulers };
    editor.save();
}
const error = computed(() => {
    void version.value;
    return node.value.address ? edits.error(node.value.address) || node.value.constructionError : "";
});
const curveStatus = computed(() => error.value || (editable.value && curve.value?.sampleRate
    ? "Click Add point" : "Read-only curve"));
function pointStatus(point: AutomationPoint) {
    const { id, sampleIndex, seconds, value } = point;
    return automationPointStatus({ id, sampleIndex, seconds, value }) + ` · ⇧Click Delete · ${rightClickHint} Segment`;
}
function pointerAxes(): CurveAxes | null {
    if (!surface.value || !axes.value) return null;
    const rect = surface.value.getBoundingClientRect();
    return { ...axes.value, left: rect.left, top: rect.top, width: rect.width, height: rect.height };
}
function start(event: PointerEvent, id: number, kind: CurveDragKind = "point") {
    if (!editable.value || event.button !== 0) return;
    if (event.shiftKey) {
        if (kind === "point") preparePointClick(event, id);
        return;
    }
    const axes = pointerAxes();
    if (axes && node.value.address) {
        const control = event.currentTarget as SVGElement;
        control.focus({ preventScroll: true });
        editor?.startAutomation(event, node.value.address, id, axes, kind, kind === "point" ? () => {
            if (!control.isConnected) return null;
            const rect = control.getBoundingClientRect();
            const x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
            return document.elementFromPoint(x, y)?.closest(".curve-point") === control ? { x, y } : null;
        } : undefined);
    }
}
function preparePointClick(event: PointerEvent, pointId?: number) {
    if (!editable.value || event.button !== 0 || event.shiftKey !== (pointId !== undefined)) return;
    pointClick = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, pointId };
}
function cancelPointClick() { pointClick = null; }
function applyPointClick(event: MouseEvent, pointId?: number) {
    const press = pointClick;
    pointClick = null;
    // A click synthesized after pointer lock ends cannot edit the point
    // underneath the restored cursor. The press must have chosen this action.
    if (!press || press.pointId !== pointId || event.button !== 0 || event.shiftKey !== (pointId !== undefined)
        || (event instanceof PointerEvent && press.pointerId !== event.pointerId)
        || Math.hypot(press.x - event.clientX, press.y - event.clientY) > 5 || !editable.value || !node.value.address) return;
    if (pointId !== undefined) {
        event.preventDefault();
        removePoint(pointId);
        return;
    }
    if (!curve.value?.sampleRate) return;
    const axes = pointerAxes();
    if (!axes) return;
    event.preventDefault();
    event.stopPropagation();
    void edits.add(node.value.address, curvePointAt(axes, press, curve.value.sampleRate));
}
function removePoint(id: number) {
    if (!editable.value || !node.value.address) return;
    if (segmentMenu.value?.id === id) closeMenus(true);
    void edits.remove(node.value.address, id);
}
function pointKey(event: KeyboardEvent, point: AutomationPoint) {
    if (event.key === "Delete") {
        event.preventDefault();
        removePoint(point.id);
    } else if (event.key === "Enter" || event.key === "ContextMenu" || (event.shiftKey && event.key === "F10"))
        editSegment(event, point);
}
function tensionKey(event: KeyboardEvent, point: AutomationPoint) {
    const adjustment = { ArrowLeft: -.05, ArrowDown: -.05, ArrowRight: .05, ArrowUp: .05 }[event.key];
    if (adjustment === undefined) { pointKey(event, point); return; }
    event.preventDefault();
    event.stopPropagation();
    changeSegment(point, "bezier", Math.min(1, Math.max(0, point.tension + adjustment)));
}

let observer: ResizeObserver | null = null;
let unregisterValueNavigation: (() => void) | undefined;
function canPanValues() {
    const axis = axes.value;
    return !!axis && edits.pointerId() === null && (axis.min === undefined || axis.max === undefined);
}
function setValuePan(nodeKey: string, pan: number) {
    if (!editor) return;
    const automationValuePans = { ...editor.layout.value.automationValuePans };
    if (pan) automationValuePans[nodeKey] = pan;
    else delete automationValuePans[nodeKey];
    editor.layout.value = { ...editor.layout.value, automationValuePans };
}
const valueNavigation: LaneValueNavigation = {
    begin() {
        const axis = axes.value, nodeKey = key.value;
        if (!editor || nodeKey === null || !axis || !canPanValues()) return null;
        const initial = valuePan.value;
        let pan = initial;
        const laneHeight = height.value;
        return {
            move(dy) {
                const next = panAutomationValueAxis({ ...axis, pan }, dy, laneHeight);
                if (next !== pan) setValuePan(nodeKey, pan = next);
            },
            changed() { return pan !== initial; },
        };
    },
    reset() {
        if (editor && key.value !== null && valuePan.value && canPanValues()) {
            setValuePan(key.value, 0);
            editor.save();
        }
    },
};
onMounted(() => {
    window.addEventListener("pointerdown", outside, true);
    observer = new ResizeObserver(([entry]) => {
        if (entry) { width.value = Math.max(1, entry.contentRect.width); height.value = Math.max(1, entry.contentRect.height); }
    });
    if (surface.value) observer.observe(surface.value);
    const lane = surface.value?.closest(".lane");
    if (lane) unregisterValueNavigation = navigation?.registerValueNavigation(lane, valueNavigation);
});
onBeforeUnmount(() => {
    unregisterValueNavigation?.();
    observer?.disconnect();
    window.removeEventListener("pointerdown", outside, true);
});
</script>

<template>
    <NodeLane :mode="mode">
        <div class="automation-surface" :data-mode="mode">
            <RulerGraduations class="clip-ruler" :ticks="ticks" :label-inset="valueLabelInset" compact />
            <svg ref="surface" class="automation-curve" :viewBox="`0 0 ${width} ${height}`" preserveAspectRatio="none"
                :aria-label="node.displayName || 'Automation curve'" :aria-description="curveStatus"
                :data-status-hint="curveStatus" @pointerdown.self="preparePointClick($event)"
                @pointercancel="cancelPointClick" @click.self="applyPointClick($event)">
                <g class="value-guides" aria-hidden="true">
                    <g v-for="guide in valueGuides" :key="guide.value" :class="{ zero: guide.value === 0, unit: Math.abs(guide.value) === 1 }">
                        <line :x1="valueLabelInset" :x2="width" :y1="guide.y" :y2="guide.y" />
                        <text v-if="mode !== 'nano'" x="4" :y="Math.min(height - 1, Math.max(8, guide.y + 3))">{{ guide.value }}</text>
                    </g>
                </g>
                <path :d="path" class="curve-line" />
                <TimelinePlayhead />
                <g v-for="point in handles" :key="`tension-${point.id}`" class="curve-tension"
                    :transform="`translate(${pointX(point)} ${handleY(point)})`"
                    role="slider" tabindex="0" aria-label="Bézier tension" :aria-valuemin="0" :aria-valuemax="1"
                    :aria-valuenow="point.tension" :data-status-hint="`Tension ${Number(point.tension.toFixed(2))} · ↔ Drag`"
                    @pointerdown="start($event, point.id, 'tension')" @focus="selectPoint(point.id)" @click.stop
                    @keydown="tensionKey($event, point)" @contextmenu.stop="editSegment($event, point)">
                    <path class="tension-stem" :d="`M0 0 V${markerY(point) - handleY(point)}`" />
                    <circle class="control-target" r="6" />
                    <path class="tension-marker" d="M0 -3.4 L3.4 0 L0 3.4 L-3.4 0Z"
                        :fill-opacity=".25 + point.tension * .75" />
                </g>
                <g v-for="point in markers" :key="point.id" class="curve-point"
                    :transform="`translate(${pointX(point)} ${markerY(point)})`" role="button" tabindex="0"
                    :data-selected="selectedId === point.id || undefined"
                    :data-limited="eventValue(point) !== point.value || undefined"
                    :aria-label="`Point at ${point.sampleIndex ?? point.seconds} ${point.sampleIndex === undefined ? 'seconds' : 'samples'}, value ${point.value}`"
                    :data-status-hint="pointStatus(point)"
                    aria-description="Drag to move. Shift-click to delete. Right-click for segment options."
                    @pointerdown="start($event, point.id)" @focus="selectPoint(point.id)"
                    @click.stop="applyPointClick($event, point.id)"
                    @keydown="pointKey($event, point)" @contextmenu.stop="editSegment($event, point)">
                    <circle class="control-target" r="8" />
                    <circle class="point-marker" r="2.6" />
                </g>
            </svg>
            <button v-if="editable && editor && curve" ref="scaleButton" type="button" class="automation-scale-button"
                aria-label="Automation value scale" aria-haspopup="dialog" :aria-expanded="!!scaleMenu"
                data-status-hint="Set value bounds and snapping" @pointerdown.stop @click.stop="showScale">Scale</button>
        </div>
    </NodeLane>
    <Teleport to="body">
        <div v-if="editable && segmentMenu && selected" ref="popover" class="automation-popover"
            :style="{ left: `${segmentMenu.left}px`, top: `${segmentMenu.top}px` }"
            role="dialog" aria-label="Automation segment" @keydown.esc.stop="closeMenus(true)" @pointerdown.stop @wheel.stop>
            <label>Segment <select :value="selected.interpolation"
                @change="changeSegment(selected, ($event.target as HTMLSelectElement).value as CurveInterpolation, selected.tension)">
                <option value="empty">Empty</option><option value="hold">Hold</option><option value="bezier">Bézier</option>
            </select></label>
            <label v-if="selected.interpolation === 'bezier'">Tension
                <input type="number" min="0" max="1" step="any" :value="selected.tension"
                    @change="changeSegment(selected, selected.interpolation, ($event.target as HTMLInputElement).valueAsNumber)" />
            </label>
            <button type="button" class="delete-point" @click="removePoint(selected.id)">Delete point</button>
        </div>
        <div v-if="editable && scaleMenu" ref="popover" class="automation-popover"
            :style="{ left: `${scaleMenu.left}px`, top: `${scaleMenu.top}px` }"
            role="dialog" aria-label="Automation value scale" @keydown.esc.stop="escapeScale"
            @pointerdown.stop @wheel.stop>
            <div v-for="bound in boundControls" :key="bound.side" class="automation-bound" :class="{ inactive: !bound.enabled }">
                <span>{{ bound.name }}</span>
                <div class="automation-bound-control">
                    <span class="automation-bound-value">{{ Number(bound.value.toPrecision(6)) }}</span>
                    <button type="button" class="automation-bound-knob" role="slider" :aria-label="bound.name"
                        :aria-valuenow="bound.value" :aria-valuemin="bound.limits.min" :aria-valuemax="bound.limits.max"
                        :disabled="!bound.enabled" :aria-disabled="!bound.enabled || boundsPending"
                        :data-status-hint="`${bound.name} ${Number(bound.value.toPrecision(6))} · Drag`"
                        @pointerdown="startBound($event, bound.side)"
                        @keydown.up.stop.prevent="stepBound(bound.side, 1)" @keydown.right.stop.prevent="stepBound(bound.side, 1)"
                        @keydown.down.stop.prevent="stepBound(bound.side, -1)" @keydown.left.stop.prevent="stepBound(bound.side, -1)">
                        <KnobFace :value="bound.value" :limits="bound.limits" />
                    </button>
                    <button type="button" class="knob-side-toggle" :aria-pressed="bound.enabled" :aria-disabled="boundsPending"
                        :aria-label="`${bound.name} bound`" :data-status-hint="`${bound.enabled ? 'Disable' : 'Enable'} ${bound.name.toLowerCase()} bound`"
                        @click="toggleBound(bound.side)">
                        <span aria-hidden="true">{{ bound.enabled ? '●' : '○' }}</span>
                    </button>
                </div>
            </div>
            <label>Value snapping
                <select :value="ruler" data-status-hint="Auto adds finer guides as you zoom in" @change="changeRuler">
                    <option value="auto">Auto</option>
                    <option value="whole">Whole</option>
                    <option value="half">Halves</option>
                    <option value="off">Off</option>
                </select>
            </label>
            <span v-if="error" role="alert" class="scale-error">{{ error }}</span>
        </div>
    </Teleport>
</template>

<style scoped>
.automation-surface { position: relative; width: 100%; height: 100%; min-height: 1px; color: var(--iv-knob-input); }
.clip-ruler { opacity: .55; }
.automation-surface[data-mode="nano"] :deep(.sample-label) { display: none; }
.automation-curve { position: relative; display: block; width: 100%; height: 100%; overflow: hidden; touch-action: none; }
.value-guides { pointer-events: none; }
.value-guides line { stroke: var(--vscode-foreground); stroke-opacity: .08; }
.value-guides .unit line { stroke-opacity: .15; }
.value-guides .zero line { stroke-opacity: .22; }
.value-guides text { fill: var(--vscode-descriptionForeground); opacity: .75; font-size: 9px;
    font-variant-numeric: tabular-nums; }
.curve-line { fill: none; stroke: currentColor; stroke-width: 1.5; pointer-events: none; }
.curve-point { cursor: grab; }
.curve-tension { cursor: ew-resize; }
.curve-point:focus, .curve-tension:focus { outline: none; }
.control-target { fill: transparent; }
.point-marker, .tension-marker { fill: currentColor; pointer-events: none; }
.curve-point[data-limited] .point-marker { fill-opacity: .6; }
.curve-point:is(:focus-visible, :has([data-hovered]), [data-hovered], [data-selected], :active) .point-marker { r: 3.6px; }
.tension-stem { fill: none; stroke: currentColor; stroke-opacity: .22; pointer-events: none; }
.curve-tension:is(:focus-visible, :has([data-hovered]), [data-hovered], :active) .tension-marker { fill-opacity: 1; }
.curve-point:focus-visible .point-marker, .curve-tension:focus-visible .tension-marker {
    stroke: var(--vscode-focusBorder, currentColor);
    stroke-width: 1px;
}
.automation-scale-button {
    position: absolute; top: 2px; right: 4px; border: 1px solid transparent; padding: 1px 4px; border-radius: 3px;
    color: var(--vscode-descriptionForeground); background: var(--vscode-editor-background); font: inherit; font-size: 10px;
    cursor: pointer;
}
.automation-scale-button:is(:hover, :focus-visible, [aria-expanded="true"]) {
    color: var(--vscode-foreground); border-color: var(--vscode-panel-border);
}
.automation-popover {
    position: fixed; z-index: 1000; display: grid; gap: 8px; padding: 10px; max-width: 192px;
    color: var(--vscode-editorWidget-foreground); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border); box-shadow: 0 2px 8px var(--vscode-widget-shadow);
}
.automation-popover label { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.automation-popover select, .automation-popover input {
    max-width: 100px; color: var(--vscode-input-foreground); background: var(--vscode-input-background);
    border: 1px solid var(--vscode-input-border); font: inherit;
}
.automation-popover .delete-point { justify-self: end; color: var(--vscode-errorForeground); background: transparent;
    border: 0; border-radius: 2px; font: inherit; padding: 3px 8px; cursor: pointer; }
.automation-popover .delete-point:is(:hover, :focus-visible) { background: var(--vscode-toolbar-hoverBackground); }
.automation-bound { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.automation-bound.inactive { color: var(--vscode-descriptionForeground); }
.automation-bound-control { display: flex; align-items: center; --knob-toggle-accent: var(--iv-knob-input); }
.automation-bound-value { margin-right: 4px; font-variant-numeric: tabular-nums; }
.automation-bound-knob {
    width: 30px; height: 30px; padding: 0; border: 0; border-radius: 50%; background: transparent;
    color: var(--knob-toggle-accent); cursor: ns-resize; touch-action: none;
}
.automation-bound-knob svg { display: block; width: 100%; height: 100%; }
.automation-bound-knob:disabled { cursor: default; opacity: .4; }
.automation-bound-knob:focus-visible { outline: 1px solid var(--knob-toggle-accent); }
.scale-error { color: var(--vscode-errorForeground); }
</style>

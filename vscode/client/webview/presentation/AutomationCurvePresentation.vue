<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from "vue";
import { automationCurveBounds, automationWindow, curvePointAt, type CurveAxes } from "../../src/automationCurveEditing";
import { useAutomationEditing } from "./automationEditingContext";
import type { AutomationPoint, CurveInterpolation } from "../../src/automationCurveGeometry";
import { useNode } from "./nodeContext";
import { useTimelineWindow } from "../timeline/timelineContext";
import NodeLane from "./NodeLane.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";
import type { NodeDisplayMode } from "./displayMode";

defineProps<{ mode: NodeDisplayMode }>();
const node = useNode();
const { edits, version, begin } = useAutomationEditing();
const time = useTimelineWindow();
const surface = ref<SVGSVGElement | null>(null);
const width = ref(1), height = ref(1);
const curve = computed(() => { void version.value; return node.value.address ? edits.view(node.value.address) : null; });
const axes = computed(() => {
    if (!curve.value || !node.value.address) return null;
    const fixed = edits.axes(node.value.address);
    const values = automationCurveBounds(curve.value);
    const padding = (values.max - values.min) * .05;
    const bounds = fixed ?? { min: values.min - padding, max: values.max + padding };
    // Without a shared timeline, frame the stored points; the curve itself is unbounded.
    const points = curve.value.points;
    const window = time?.value ?? (points.length && points.at(-1)!.seconds > points[0].seconds
        ? { beginSeconds: points[0].seconds, endSeconds: points.at(-1)!.seconds }
        : { beginSeconds: (points[0]?.seconds ?? 0) - .5, endSeconds: (points[0]?.seconds ?? 0) + .5 });
    return { begin: window.beginSeconds, end: window.endSeconds, min: bounds.min, max: bounds.max };
});
const view = computed(() => {
    void version.value;
    return curve.value && axes.value && node.value.address
        ? automationWindow(curve.value, axes.value.begin, axes.value.end, edits.drafts(node.value.address)) : null;
});
const x = (seconds: number) => axes.value ? (seconds - axes.value.begin) / (axes.value.end - axes.value.begin) * width.value : 0;
const y = (value: number) => axes.value ? (axes.value.max - value) / (axes.value.max - axes.value.min) * height.value : 0;
const path = computed(() => view.value?.paths.map((segment) => {
    const a = x(segment.begin.seconds), b = x(segment.end.seconds);
    return `M${a},${y(segment.begin.value)} C${a + (b - a) / 3},${y(segment.first)} ${b - (b - a) / 3},${y(segment.second)} ${b},${y(segment.end.value)}`;
}).join(" ") ?? "");
const editing = ref<{ id: number; left: number; top: number } | null>(null);
const segmentPopover = ref<HTMLElement | null>(null);
const selected = computed<AutomationPoint | null>(() => curve.value?.points.find((point) => point.id === editing.value?.id) ?? null);
function editSegment(event: MouseEvent, point: AutomationPoint) {
    event.preventDefault(); event.stopPropagation();
    editing.value = { id: point.id, left: Math.max(4, Math.min(window.innerWidth - 220, event.clientX)),
        top: Math.max(4, Math.min(window.innerHeight - 125, event.clientY)) };
    void nextTick(() => segmentPopover.value?.querySelector<HTMLSelectElement>("select")?.focus({ preventScroll: true }));
}
function changeSegment(interpolation: CurveInterpolation, tension: number) {
    if (!node.value.address || !selected.value || !Number.isFinite(tension) || tension < 0 || tension > 1) return;
    void edits.segment(node.value.address, selected.value.id, interpolation, tension);
}
function outside(event: PointerEvent) {
    if (!(event.target instanceof Element) || !event.target.closest(".automation-point-popover")) editing.value = null;
}
const error = computed(() => { void version.value; return node.value.address ? edits.error(node.value.address) || node.value.constructionError : ""; });
function pointerAxes(): CurveAxes | null {
    if (!surface.value || !axes.value) return null;
    const rect = surface.value.getBoundingClientRect();
    return { ...axes.value, left: rect.left, top: rect.top, width: rect.width, height: rect.height };
}
function start(event: PointerEvent, id: number) {
    const axes = pointerAxes();
    if (axes && node.value.address) begin(event, node.value.address, id, axes);
}
function add(event: MouseEvent) {
    if (event.button !== 0 || !node.value.address || !curve.value) return;
    const axes = pointerAxes();
    if (!axes) return;
    event.preventDefault(); event.stopPropagation();
    void edits.add(node.value.address, curvePointAt(axes, { x: event.clientX, y: event.clientY }));
}
function remove(event: MouseEvent, id: number) {
    event.preventDefault(); event.stopPropagation();
    if (node.value.address) void edits.remove(node.value.address, id);
}
let observer: ResizeObserver | null = null;
onMounted(() => {
    window.addEventListener("pointerdown", outside, true);
    observer = new ResizeObserver(() => {
        if (surface.value) { width.value = Math.max(1, surface.value.clientWidth); height.value = Math.max(1, surface.value.clientHeight); }
    });
    if (surface.value) observer.observe(surface.value);
});
onBeforeUnmount(() => { observer?.disconnect(); window.removeEventListener("pointerdown", outside, true); }); // Gesture ownership stays in the webview.
</script>

<template>
    <NodeLane :mode="mode">
        <LiveOutputMeters v-if="mode === 'mini' || mode === 'nano'" :mode="mode"
            :bounds="axes ? { min: axes.min, max: axes.max } : undefined" />
        <svg v-show="mode !== 'mini' && mode !== 'nano'" ref="surface" class="automation-curve" :viewBox="`0 0 ${width} ${height}`"
            :aria-label="`${node.displayName || 'Automation curve'}; double-click to add a point`" @dblclick="add">
            <title>{{ error || 'Double-click to add a point or edit a point’s segment. Drag a point to move it. Right-click a point to remove it.' }}</title>
            <path :d="path" class="curve-line" />
            <circle v-for="point in view?.points ?? []" :key="point.id" :cx="x(point.seconds)" :cy="y(point.value)"
                r="5" class="curve-point" role="button" :aria-label="`Point at ${point.seconds} seconds: ${point.value}`"
                @pointerdown="start($event, point.id)" @dblclick.stop="editSegment($event, point)" @contextmenu="remove($event, point.id)">
                <title>{{ `${point.seconds} s: ${point.value}` }}</title>
            </circle>
        </svg>
    </NodeLane>
    <Teleport to="body">
        <div v-if="editing && selected" ref="segmentPopover" class="automation-point-popover"
            :style="{ left: `${editing.left}px`, top: `${editing.top}px` }"
            role="dialog" aria-label="Automation segment" @keydown.esc="editing = null" @pointerdown.stop @wheel.stop>
            <label>Segment <select :value="selected.interpolation"
                @change="changeSegment(($event.target as HTMLSelectElement).value as CurveInterpolation, selected.tension)">
                <option value="empty">Empty</option><option value="hold">Hold</option><option value="bezier">Bézier</option>
            </select></label>
            <label v-if="selected.interpolation === 'bezier'">Tension
                <input type="number" min="0" max="1" step="0.05" :value="selected.tension"
                    @change="changeSegment(selected.interpolation, Number(($event.target as HTMLInputElement).value))" />
            </label>
        </div>
    </Teleport>
</template>

<style scoped>
.automation-curve { display: block; width: 100%; height: 100%; min-height: 1px; overflow: hidden; touch-action: none; }
.curve-line { fill: none; stroke: var(--vscode-charts-yellow, #e2c36b); stroke-width: 1.5; pointer-events: none; }
.curve-point { fill: var(--vscode-editor-background); stroke: var(--vscode-charts-yellow, #e2c36b); stroke-width: 2; cursor: grab; }
.curve-point[data-hovered] { fill: var(--vscode-charts-yellow, #e2c36b); }
.automation-point-popover { position: fixed; z-index: 1000; display: grid; gap: 8px; padding: 10px;
    color: var(--vscode-editorWidget-foreground); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border); box-shadow: 0 2px 8px var(--vscode-widget-shadow); }
.automation-point-popover label { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.automation-point-popover select, .automation-point-popover input { max-width: 100px;
    color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border); }
</style>

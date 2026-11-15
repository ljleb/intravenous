<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { automationCurveBounds, automationWindow, curvePointAt, type CurveAxes } from "../../src/automationCurveEditing";
import { useAutomationEditing } from "./automationEditingContext";
import { useNode } from "./nodeContext";
import { useTimelineWindow } from "../timeline/timelineContext";
import NodeLane from "./NodeLane.vue";
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
const path = computed(() => view.value?.vertices.map((point, index) => `${index ? "L" : "M"}${x(point.seconds)},${y(point.value)}`).join(" ") ?? "");
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
    observer = new ResizeObserver(() => {
        if (surface.value) { width.value = Math.max(1, surface.value.clientWidth); height.value = Math.max(1, surface.value.clientHeight); }
    });
    if (surface.value) observer.observe(surface.value);
});
onBeforeUnmount(() => observer?.disconnect()); // Gesture ownership stays in the webview.
</script>

<template>
    <NodeLane :mode="mode">
        <svg ref="surface" class="automation-curve" :viewBox="`0 0 ${width} ${height}`"
            :aria-label="`${node.displayName || 'Automation curve'}; double-click to add a point`" @dblclick="add">
            <title>{{ error || 'Double-click to add a point. Drag a point to move it. Right-click a point to remove it.' }}</title>
            <path :d="path" class="curve-line" />
            <circle v-for="point in view?.points ?? []" :key="point.id" :cx="x(point.seconds)" :cy="y(point.value)"
                r="5" class="curve-point" role="button" :aria-label="`Point at ${point.seconds} seconds: ${point.value}`"
                @pointerdown="start($event, point.id)" @dblclick.stop @contextmenu="remove($event, point.id)">
                <title>{{ `${point.seconds} s: ${point.value}` }}</title>
            </circle>
        </svg>
    </NodeLane>
</template>

<style scoped>
.automation-curve { display: block; width: 100%; height: 100%; min-height: 1px; overflow: hidden; touch-action: none; }
.curve-line { fill: none; stroke: var(--vscode-charts-yellow, #e2c36b); stroke-width: 1.5; pointer-events: none; }
.curve-point { fill: var(--vscode-editor-background); stroke: var(--vscode-charts-yellow, #e2c36b); stroke-width: 2; cursor: grab; }
.curve-point[data-hovered] { fill: var(--vscode-charts-yellow, #e2c36b); }
</style>

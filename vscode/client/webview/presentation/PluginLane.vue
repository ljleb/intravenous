<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef, watch } from "vue";
import { addressKey } from "../../src/graphScope";
import { presentationNodeEvent, type JsonValue } from "../../src/nodeEvents";
import type { NodeEventGesture } from "../../src/nodeEventMessaging";
import { PresentationEdits, type PresentationEdit } from "../../src/presentationEdits";
import type { PresentationFrame, PresentationInput, PresentationModifiers, PresentationObservation, PresentationPoint,
    PresentationProgram, PresentationSnapshot } from "../../src/presentationPlugin";
import { PresentationPluginSession } from "../../src/presentationPluginSession";
import { sampleWindow } from "../../src/sampleWaveform";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineWindow } from "../timeline/timelineContext";
import { usePlayheadSeek } from "../timeline/usePlayheadSeek";
import GenericPresentation from "./GenericPresentation.vue";
import NodeLane from "./NodeLane.vue";
import LaneSettings from "./LaneSettings.vue";
import PresentationControls from "./PresentationControls.vue";
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";
import { usePresentationActive, usePortObservationViewport } from "./observationContext";
import { usePresentationFrame } from "./presentationFrames";
import { usePresentationSize } from "./presentationSize";
import { usePresentationState } from "./presentationStateContext";
import { PresentationPluginWorker } from "./presentationPluginWorker";

const props = defineProps<{ mode: NodeDisplayMode; program: PresentationProgram }>();
const node = useNode();
const editor = useOptionalGraphEditor();
const time = useTimelineWindow();
const surface = ref<HTMLElement | null>(null);
const size = usePresentationSize(surface);
const frame = shallowRef<PresentationFrame>();
const state = shallowRef<JsonValue>(null);
const error = ref("");
const editError = ref("");
const editable = computed(() => props.mode === "editable" && !!editor?.scope.value?.editable
    && !!node.value.address && node.value.loadState === "ready");
const worker = "source" in props.program ? new PresentationPluginWorker(props.program.source) : undefined;
const present = "create" in props.program ? props.program.create() : undefined;
let cancelDrag: (() => void) | undefined;
const dragging = ref(false);
const panelOpen = ref(false);
const active = usePresentationActive(() => dragging.value || panelOpen.value);
const save = usePresentationState({
    serialize: () => state.value === null ? null : { plugin: state.value },
    restore: (saved) => { state.value = saved?.plugin ?? null; },
});
const observed = (props.program.manifest.observations ?? []).map((need) => ({ id: need.id, view: usePortObservationViewport(() => {
    const { address, ports } = node.value;
    const timeline = time?.value;
    if (!address || !timeline || !ports.some((port) => port.direction === need.direction && port.name === need.port && port.kind === "continuous")) return;
    const window = sampleWindow(timeline.beginSeconds, timeline.endSeconds, timeline.sampleRate, need.points);
    if (window) return { source: { node: address, kind: need.direction, member: need.port }, operation: "waveform", window };
}) }));
const observations = computed(() => Object.fromEntries(observed.map(({ id, view }): [string, PresentationObservation | null] => {
    const observation = view.value;
    if (!observation) return [id, null];
    const spans = observation.spans.map(({ begin, end, result }) => ({ begin: String(begin), end: String(end), result }));
    return [id, { ...observation, spans }];
})));
const gestures = new Map<string, NodeEventGesture>();
const editsPending = ref(false);
const acceptedRevision = ref(0);
let idleTimer: ReturnType<typeof setTimeout> | undefined;
let disposed = false;

const edits = new PresentationEdits(sendEdit, () => { editsPending.value = edits.pending; }, (reason) => {
    editError.value = String(reason).slice(0, 512);
    cancelDrag?.();
    closeGestures();
});

async function sendEdit(command: PresentationEdit): Promise<void> {
    if (command.type === "finish") {
        const gesture = gestures.get(command.gesture);
        if (gesture) {
            gestures.delete(command.gesture);
            await gesture.finish();
        }
        return;
    }
    if (!editable.value) return;
    editError.value = "";
    const key = command.gesture;
    let gesture = key ? gestures.get(key) : undefined;
    if (!gesture) {
        if (key && gestures.size >= 8) throw new Error("Too many open presentation gestures");
        gesture = node.value.gesture();
        if (key) gestures.set(key, gesture);
    }
    try {
        await gesture.send(presentationNodeEvent(command.event));
        acceptedRevision.value = Math.max(acceptedRevision.value, gesture.acceptedRevision?.() ?? 0);
    } finally { if (!key) await gesture.finish(); }
}

function closeGestures() {
    for (const gesture of gestures.values()) void gesture.finish().catch(() => {});
    gestures.clear();
}
function fail(reason: unknown) {
    if (disposed) return;
    error.value = String(reason).slice(0, 512);
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    closeGestures();
    worker?.stop();
}
function accept(next: PresentationFrame, draw: boolean, hasInput: boolean) {
    if (hasInput) for (const command of next.commands ?? []) {
        if (command.type !== "finish" && !editable.value) continue;
        if (command.type === "state") {
            state.value = command.value;
            save();
        } else edits.enqueue(command);
    }
    if (active.value && draw) frame.value = next;
}
const job = usePresentationFrame("update", () => { void session.flush(); });
const session = new PresentationPluginSession((snapshot, inputs) => {
    if (present) return present(snapshot, inputs);
    return worker!.run(JSON.parse(JSON.stringify(snapshot)), inputs);
}, job.schedule, accept, fail);

watch((): PresentationSnapshot | undefined => {
    if (error.value || !active.value || !size.value?.width || !size.value.height) return;
    const timeline = time?.value;
    const scopedNode = node.value;
    const revision = editor?.scope.value?.sessionRevision ?? 0;
    return { node: { definitionId: scopedNode.definitionId, arguments: scopedNode.construction?.arguments ?? [],
        ports: scopedNode.ports.map(({ name, direction, kind }) => ({ name, direction, kind })) },
        revision, editable: editable.value, mode: props.mode, editsPending: editsPending.value || revision < acceptedRevision.value,
        editError: editError.value || undefined,
        viewport: { ...size.value, ...(timeline ? { time: { beginSeconds: timeline.beginSeconds,
            spanSeconds: timeline.spanSeconds ?? timeline.endSeconds - timeline.beginSeconds, sampleRate: timeline.sampleRate ?? undefined } } : {}) },
        observations: observations.value,
        state: state.value };
}, (snapshot) => {
    clearTimeout(idleTimer);
    // In-process programs can project this viewport before Vue draws it.
    // Deferring that work by another frame leaves their marks in the old view.
    if (snapshot) session.update(snapshot, present ? "immediate" : "frame");
    else if (!disposed) {
        // The next viewport replaces this snapshot. Keep the last frame for
        // display and release a worker that repeatedly goes off screen.
        session.suspend();
        if (worker) idleTimer = setTimeout(() => { if (!active.value && !editsPending.value) worker.stop(); }, 2000);
    }
}, { immediate: true });

const seek = usePlayheadSeek(surface, () => frame.value?.seekMarks ? time?.value : undefined,
    () => frame.value?.seekMarks?.map(BigInt) ?? [], () => {
        const read = present?.openSeekReader?.();
        if (!read) return;
        let cached: { first: bigint; length: bigint; marks: bigint[] } | undefined;
        return { marks({ first, length }, sampleRate, width) {
            if (cached?.first !== first || cached.length !== length) {
                cached = { first, length, marks: read({ first: String(first), length: String(length), sampleRate, width }).map(BigInt) };
            }
            return cached.marks;
        } };
    });
function modifiers(event: MouseEvent | KeyboardEvent): PresentationModifiers {
    return { shift: event.shiftKey, alt: event.altKey, control: event.ctrlKey, meta: event.metaKey };
}
function pointerDown(event: PointerEvent) {
    if (event.button !== 0 || cancelDrag || !surface.value || !editor || error.value) return;
    const box = surface.value.getBoundingClientRect();
    let at = { x: event.clientX - box.left, y: event.clientY - box.top };
    const targets = editable.value ? frame.value?.targets ?? [] : [];
    const target = [...targets].reverse().find((item) => at.x >= item.x && at.x <= item.x + item.width
        && at.y >= item.y && at.y <= item.y + item.height);
    if (!target) { seek(event); return; }
    let held = modifiers(event);
    const send = (phase: Extract<PresentationInput, { type: "gesture" }>["phase"], delta?: PresentationPoint) =>
        session.input({ type: "gesture", id: target.id, phase, point: at, delta, modifiers: held });
    const move = (point: PresentationPoint) => {
        const delta = { x: point.x - at.x, y: point.y - at.y };
        if (delta.x || delta.y) { at = point; send("move", delta); }
    };
    cancelDrag = editor.startObjectDrag(event, {
        move(point) { move({ x: point.x - box.left, y: point.y - box.top }); },
        moveBy(delta) { move({ x: at.x + delta.x, y: at.y + delta.y }); },
        modifiers(event) {
            const next = modifiers(event);
            if (next.shift === held.shift && next.alt === held.alt && next.control === held.control && next.meta === held.meta) return;
            held = next;
            send("modifiers");
        },
        finish(point, lockOrigin) {
            if (point && !lockOrigin) move({ x: point.x - box.left, y: point.y - box.top });
            send(point ? "finish" : "cancel");
            cancelDrag = undefined;
            dragging.value = false;
            return undefined;
        },
    });
    if (cancelDrag) {
        dragging.value = true;
        send("begin");
    }
}
function control(input: PresentationInput) {
    if (!error.value && (editable.value || (input.type === "control" && input.phase === "finish"))) session.input(input);
}
function svgPoints(points: PresentationPoint[]) { return points.map(({ x, y }) => `${x},${y}`).join(" "); }
onBeforeUnmount(() => {
    disposed = true;
    clearTimeout(idleTimer);
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    worker?.stop();
    closeGestures();
});
</script>

<template>
    <NodeLane v-if="!program.manifest.timeline || time" :mode="mode" flush-content>
        <div ref="surface" class="plugin-surface" :data-status-hint="error || editError || frame?.status"
            :class="{ 'plugin-seek': frame?.seekMarks }" :data-status-shortcuts="frame?.seekMarks ? 'Click/drag seek · Space ▶/Ⅱ' : undefined"
            @pointerdown.self="pointerDown">
            <svg class="plugin-scene" aria-hidden="true">
                <template v-for="mark in frame?.marks" :key="mark.id">
                    <polyline v-if="mark.type === 'polyline'" :points="svgPoints(mark.points)" :data-ink="mark.ink" :opacity="mark.opacity" />
                    <rect v-else-if="mark.type === 'rect'" :x="mark.x" :y="mark.y" :width="mark.width" :height="mark.height"
                        :data-ink="mark.ink" :opacity="mark.opacity" />
                    <text v-else :x="mark.x" :y="mark.y" :data-ink="mark.ink" :opacity="mark.opacity">{{ mark.text }}</text>
                </template>
            </svg>
            <LaneSettings v-if="frame?.panel && node.address && mode === 'editable' && !error" :label="frame.panel.label"
                dialog-label="Lane settings" :hint="editError || frame.status || frame.panel.label" :width="236" :preferred-height="260" @toggle="panelOpen = $event">
                <PresentationControls :rows="frame.panel.rows" :identity="addressKey(node.address)" :editable="editable" @input="control" />
            </LaneSettings>
            <span v-if="error" class="plugin-error" role="status">{{ error }}</span>
        </div>
    </NodeLane>
    <GenericPresentation v-else :mode="mode" />
</template>

<style scoped>
.plugin-surface { position: relative; width: 100%; height: 100%; overflow: hidden; touch-action: none; user-select: none; }
.plugin-seek { cursor: ew-resize; }
.plugin-scene { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.plugin-scene polyline { fill: none; stroke: currentColor; stroke-width: 1; }
.plugin-scene text, .plugin-scene rect { fill: currentColor; }
.plugin-scene text { font: 10px monospace; }
[data-ink="input"] { color: var(--iv-port-input); }
[data-ink="output"] { color: var(--iv-port-output); }
[data-ink="muted"] { color: var(--vscode-descriptionForeground); }
[data-ink="selection"] { color: var(--vscode-focusBorder); }
.plugin-error { position: absolute; top: 4px; left: 8px; color: var(--vscode-errorForeground); }
</style>

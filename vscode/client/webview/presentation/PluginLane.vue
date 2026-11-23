<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef, watch } from "vue";
import { addressKey } from "../../src/graphScope";
import { graphEditorShortcut, isTextEditingTarget } from "../../src/graphEditorShortcuts";
import { PresentationLaneEdits } from "../../src/presentationLaneEdits";
import type { PresentationCursor, PresentationFrame, PresentationGestureInput, PresentationInput, PresentationObservation, PresentationPoint,
    PresentationProgram, PresentationSavedState, PresentationSnapshot } from "../../src/presentationPlugin";
import { PresentationPluginSession, PresentationWake } from "../../src/presentationPluginSession";
import { openPresentationTimelineReader, renderPresentationTimelineGrid } from "../../src/presentationTimelineGrid";
import { ReloadableWasmPresentation } from "../../src/reloadableWasmPresentation";
import { sampleWindow } from "../../src/sampleWaveform";
import { timelineSamples, timelineVisibleEventSpan } from "../../src/timelineSamples";
import { recordedTriggerBinsOperation } from "../../src/observations";
import { useOptionalGraphEditor } from "../graph/context";
import { presentationCursorPoint, visibleCursorDestination } from "../graph/cursorReturn";
import { useTimelineMarks, useTimelineNavigation, useTimelineWindow, type LaneValueNavigation } from "../timeline/timelineContext";
import { usePlayback } from "../timeline/playbackContext";
import { usePlayheadSeek } from "../timeline/usePlayheadSeek";
import GenericPresentation from "./GenericPresentation.vue";
import NodeLane from "./NodeLane.vue";
import LaneSettings from "./LaneSettings.vue";
import AnchoredPopover from "./AnchoredPopover.vue";
import { useContextPopover } from "./contextPopover";
import PresentationControls from "./PresentationControls.vue";
import PresentationScene from "./PresentationScene.vue";
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";
import { usePresentationActive, usePortObservationViewports } from "./observationContext";
import { usePresentationFrame } from "./presentationFrames";
import { presentationDrag } from "./presentationDrag";
import { usePresentationSize } from "./presentationSize";
import { usePresentationState } from "./presentationStateContext";
import { createWebPresentationWorker } from "./presentationPluginWorker";

const props = defineProps<{ mode: NodeDisplayMode; program: PresentationProgram }>();
const node = useNode();
const editor = useOptionalGraphEditor();
const time = useTimelineWindow();
const timelineMarks = useTimelineMarks();
const playback = usePlayback();
const navigation = useTimelineNavigation();
const surface = ref<HTMLElement | null>(null);
const size = usePresentationSize(surface);
const frame = shallowRef<PresentationFrame>();
const state = shallowRef<PresentationSavedState>(null);
const error = ref("");
const editError = ref("");
const replacementError = ref("");
const editable = computed(() => props.mode === "editable" && !!editor?.scope.value?.editable
    && !!node.value.address && node.value.loadState === "ready");
const worker = "bytes" in props.program ? new ReloadableWasmPresentation(createWebPresentationWorker,
    (reason) => { replacementError.value = reason === undefined ? "" : String(reason).slice(0, 512); }) : undefined;
const present = "create" in props.program ? props.program.create() : undefined;
let cancelDrag: (() => void) | undefined;
const dragging = ref(false);
const panelOpen = ref(false);
const menu = useContextPopover<string>();
const { panel: contextPanel } = menu;
const editsPending = ref(false);
const waking = ref(false);
const active = usePresentationActive(() => dragging.value || panelOpen.value || menu.key.value !== null || editsPending.value || waking.value);
const save = usePresentationState({
    serialize: () => state.value,
    restore: (saved) => {
        state.value = saved ?? null;
        worker?.restore(state.value);
    },
});
const needs = props.program.manifest.observations ?? [];
const observationGroups = new Map<string, typeof needs>();
for (const need of needs) {
    const key = need.group === undefined ? `port:${need.id}` : `group:${need.group}`;
    const group = observationGroups.get(key);
    if (group) group.push(need);
    else observationGroups.set(key, [need]);
}
const observed = [...observationGroups.values()].map((group) => ({ group, view: usePortObservationViewports(group.map((need) => () => {
    const { address, ports } = node.value;
    const timeline = time?.value;
    const event = need.operation === recordedTriggerBinsOperation;
    const port = ports.find((port) => port.direction === need.direction && port.name === need.port
        && port.kind === (event ? "event" : "continuous"));
    if (!address || !timeline?.sampleRate || !port || (need.whenConnected && port.connectivity !== "connected")) return;
    const window = event ? { ...timelineVisibleEventSpan(timelineSamples(timeline)),
        points: Math.max(1, Math.min(1024, Math.floor(size.value?.width ?? 1))) } : sampleWindow(timeline, need.points);
    if (window) return { source: { node: address, kind: need.direction, member: need.port, portKind: event ? "event" : undefined },
        operation: need.operation ?? "waveform", window,
        completeOnly: need.completeOnly };
})) }));
const observations = computed(() => Object.fromEntries(observed.flatMap(({ group, view }) => group.map(({ id, operation = "waveform" }, index): [string, PresentationObservation | null] => {
    const observation = view.value[index];
    if (!observation) return [id, null];
    const spans = observation.spans.map(({ begin, end, windowOffset, result }) =>
        ({ begin: String(begin), end: String(end), windowOffset: String(windowOffset), result }));
    return [id, { ...observation, operation, spans }];
}))));
const acceptedRevision = ref(0);
const wake = new PresentationWake(() => session.input({ type: "wake" }));
let idleTimer: ReturnType<typeof setTimeout> | undefined;
let disposed = false;
let cursorReply: { input: PresentationGestureInput; resolve: (point: PresentationCursor | undefined) => void } | undefined;

function settleCursorReply(point?: PresentationCursor) {
    cursorReply?.resolve(point);
    cursorReply = undefined;
}

const laneEdits = new PresentationLaneEdits({
    editable: () => editable.value,
    gesture: () => node.value.gesture(),
    accepted: (revision) => { acceptedRevision.value = Math.max(acceptedRevision.value, revision); },
    input: (input) => session.input(input),
    setError: (message) => { editError.value = message; },
    changed: () => { editsPending.value = laneEdits.queue.pending; },
    fail: (reason) => {
        editError.value = String(reason).slice(0, 512);
        cancelDrag?.();
    },
});
const edits = laneEdits.queue;

function fail(reason: unknown) {
    if (disposed) return;
    error.value = String(reason).slice(0, 512);
    settleCursorReply();
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    laneEdits.closeGestures();
    worker?.dispose();
}
function accept(next: PresentationFrame, draw: boolean, inputs: readonly PresentationInput[]) {
    if (cursorReply && inputs.includes(cursorReply.input)) {
        const cursor = next.drag?.cursor;
        // A timeline destination can be projected into the latest viewport
        // even when the frame's drawing was superseded during the release.
        settleCursorReply(cursor && (draw || "sample" in cursor) ? cursor : undefined);
    }
    const continuing = inputs.length > 0 || waking.value;
    const deferredState = !!present && inputs.length > 0
        && inputs.every((input) => input.type === "valueView" && input.action !== "reset");
    if (continuing) laneEdits.accept(next, (value) => {
        if (!editor?.scope.value?.editable || !node.value.address) return;
        state.value = value;
        save(!deferredState);
    });
    if (active.value && draw) frame.value = next;
    if (continuing && next.wakeAfterMs !== undefined) wake.request(next.wakeAfterMs);
    waking.value = wake.pending;
    if (next.cancelDrag) cancelDrag?.();
    if (dragging.value) editor?.refreshPointerHint();
    if (draw && menu.key.value !== null && !next.contextMenu) menu.close(true);
    if (inputs.length && !dragging.value && "bytes" in props.program
        && worker?.hasReplacement(props.program.bytes) && snapshot.value) session.update(snapshot.value);
}
const job = usePresentationFrame("update", () => { void session.flush(); });
const session = new PresentationPluginSession((snapshot, inputs) => {
    if (present) return present(snapshot, inputs);
    const program = props.program;
    if (!("bytes" in program) || !worker) throw new Error("Missing presentation program");
    return worker.run(program.bytes, snapshot, inputs, () => !dragging.value && props.program === program);
}, job.schedule, accept, fail);

const snapshot = computed((): PresentationSnapshot | undefined => {
    if (error.value || !active.value || !size.value?.width || !size.value.height) return;
    const timeline = time?.value;
    const scopedNode = node.value;
    const revision = editor?.scope.value?.sessionRevision ?? 0;
    return { node: { address: scopedNode.address, definitionId: scopedNode.definitionId, arguments: scopedNode.construction?.arguments ?? [],
        ports: scopedNode.ports.map((port) => ({ name: port.name, direction: port.direction, kind: port.kind,
            connected: port.connectivity === "connected", ...(port.kind === "continuous" && port.manualControl ? {
                manualValue: port.manualControl.liveValue ?? (port.manualControl.participates ? port.manualControl.value : undefined),
            } : {}) })) },
        revision, editable: editable.value, mode: props.mode, editsPending: editsPending.value || revision < acceptedRevision.value,
        editError: editError.value || undefined,
        viewport: { ...size.value, time: timeline },
        observations: observations.value,
        state: state.value };
});
watch([snapshot, () => props.program, dragging], ([snapshot]) => {
    clearTimeout(idleTimer);
    // In-process programs can project this viewport before Vue draws it.
    // Deferring that work by another frame leaves their marks in the old view.
    if (snapshot) session.update(snapshot, present ? "immediate" : "frame");
    else if (!disposed) {
        // The next viewport replaces this snapshot. Keep the last frame for
        // display and release a worker that repeatedly goes off screen.
        session.suspend();
        if (worker) idleTimer = setTimeout(() => { if (!active.value && !editsPending.value) worker.suspend(); }, 2000);
    }
}, { immediate: true });

const timelineGrid = computed(() => {
    const grid = frame.value?.timelineGrid, view = time?.value, bounds = size.value;
    if (grid && view && bounds) return renderPresentationTimelineGrid(grid, view, bounds.width, bounds.height, props.mode === "editable");
});
const laneMarks = computed(() => timelineGrid.value
    ? [...timelineGrid.value.marks, ...frame.value?.marks ?? []] : frame.value?.marks);
const seek = usePlayheadSeek(surface, () => frame.value?.timelineGrid?.seek ? time?.value : undefined,
    () => timelineGrid.value?.samples ?? [], () => {
        const grid = frame.value?.timelineGrid;
        return grid?.seek ? openPresentationTimelineReader(grid) : undefined;
    });
const hovered = ref<string>();
const activeTarget = ref<string>();
function targetAt(point: PresentationPoint, matches: (target: NonNullable<PresentationFrame["targets"]>[number]) => boolean) {
    const targets = editable.value ? frame.value?.targets : undefined;
    if (!targets) return;
    for (let i = targets.length - 1; i >= 0; --i) {
        const item = targets[i];
        if (point.x >= item.x && point.x <= item.x + item.width && point.y >= item.y && point.y <= item.y + item.height
            && matches(item)) return item;
    }
}
function localPoint(event: MouseEvent, box: DOMRect): PresentationPoint {
    return { x: event.clientX - box.left, y: event.clientY - box.top };
}
const hoveredTarget = computed(() => frame.value?.targets?.find(({ id }) => id === (activeTarget.value ?? hovered.value)));
function hover(event: PointerEvent) {
    if (!surface.value || activeTarget.value) return;
    hovered.value = targetAt(localPoint(event, surface.value.getBoundingClientRect()), (item) => !!item.hint || !!item.cursor)?.id;
}
function pointerDown(event: PointerEvent) {
    if ((event.button !== 0 && event.button !== 2) || cancelDrag || !surface.value || !editor || error.value) return;
    const box = surface.value.getBoundingClientRect();
    const at = localPoint(event, box);
    const button = event.button === 2 ? "secondary" : "primary";
    if (button === "secondary" && targetAt(at, (item) => !!item.context)) return;
    const target = targetAt(at, (item) => (item.button ?? "primary") === button);
    if (!target) { if (button === "primary") seek(event); return; }
    const trace = target.cursorReturn ? editor.takeCursorReturnTrace() : undefined;
    trace?.log("Presentation point press", { definitionId: node.value.definitionId, targetId: target.id, button,
        pointerType: event.pointerType, local: at, screen: { x: event.screenX, y: event.screenY }, scale: window.devicePixelRatio });
    if (frame.value?.itemEditing) surface.value.focus({ preventScroll: true });
    menu.close();
    settleCursorReply();
    let released: Promise<PresentationCursor | undefined> | undefined;
    cancelDrag = editor.startObjectDrag(event, presentationDrag(event, box, target, (input) => {
        if (input.phase === "begin" && props.program.manifest.timelineSnapping && time?.value) input.snapping = {
            marks: (timelineMarks?.value ?? []).map(({ index }) => String(index)),
            playhead: playback?.backgroundPlayhead.value?.toString(),
        };
        if (input.phase === "finish" && worker && target.cursorReturn) {
            released = new Promise((resolve) => { cursorReply = { input, resolve }; });
        }
        session.input(input, present && (input.phase === "begin" || input.phase === "finish" || input.phase === "cancel") ? "immediate" : "frame");
        if (input.phase === "finish" || input.phase === "cancel") {
            cancelDrag = undefined;
            dragging.value = false;
            activeTarget.value = undefined;
            hovered.value = undefined;
        }
        if (input.phase === "begin" && present && (frame.value?.cancelDrag || error.value)) return false;
    }, {
        trace,
        hideCursor: target.hideCursor,
        hint: () => frame.value?.drag?.hint ? { text: frame.value.drag.hint, shortcuts: frame.value.drag.shortcuts } : undefined,
        cursorReturn: target.cursorReturn ? () => {
            const locate = (point: PresentationCursor | undefined) => {
                const element = surface.value;
                if (!point || !element?.isConnected) {
                    trace?.log("Presentation point has no visible destination", { point, connected: element?.isConnected ?? false });
                    return null;
                }
                const bounds = element.getBoundingClientRect();
                const requested = presentationCursorPoint(point, bounds, time?.value);
                const destination = requested && visibleCursorDestination(element, requested);
                trace?.log("Resolved presentation point destination", { point, requested, destination });
                return destination;
            };
            const point = frame.value?.drag?.cursor;
            trace?.log("Presentation release frame", { point, waitingForWorker: !!released, cancelled: frame.value?.cancelDrag ?? false });
            if (released || point) return { destination: () => released ? released.then(locate) : locate(point),
                policy: "presentationPoint" };
        } : undefined,
    }));
    if (cancelDrag) { dragging.value = true; activeTarget.value = target.id; }
}
function openContext(event: MouseEvent) {
    if (!surface.value) return;
    const point = localPoint(event, surface.value.getBoundingClientRect());
    const target = targetAt(point, (item) => !!item.context);
    if (!target) {
        if (frame.value?.itemEditing) event.preventDefault();
        return;
    }
    menu.open(event, target.id, { anchor: surface.value, returnFocus: surface.value, initialFocus: "select" });
    session.input({ type: "context", id: target.id, point }, present ? "immediate" : "frame");
}
function contextToggled(open: boolean) {
    menu.toggled(open);
    if (!open) session.input({ type: "dismissContext" });
}
function dismissContext() {
    session.input({ type: "dismissContext", cancelled: true }, present ? "immediate" : "frame");
}
const unregisterInterrupt = editor?.registerPresentationInterrupt((reason) => {
    menu.close();
    settleCursorReply();
    const replies: PresentationInput[] = [];
    for (const { requestId } of edits.discardPending()) if (requestId)
        replies.push({ type: "editResult", requestId, error: "Presentation edit interrupted" });
    session.interrupt(reason, replies, present ? "immediate" : "frame");
    cancelDrag?.();
});
watch(editable, (enabled) => { if (!enabled) menu.close(); });
function itemShortcut(event: KeyboardEvent) {
    if (!editable.value || !frame.value?.itemEditing || isTextEditingTarget(event.target)) return;
    const action = event.key === "Delete" ? "delete" : graphEditorShortcut(event) === "selectAll" ? "selectAll" : undefined;
    if (!action) return;
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat) session.input({ type: "items", action });
}
const valueNavigation: LaneValueNavigation = {
    begin(mode) {
        const element = surface.value;
        if (!element || !frame.value?.valueNavigation || dragging.value) return null;
        let changed = false;
        const base = { content: element, changed: () => changed };
        return mode === "zoom" ? { ...base, mode, zoom(factor, cursorY) {
            if (valueNavigation.zoom(factor, cursorY)) changed = true;
        } } : { ...base, mode, pan(delta) {
            if (!delta) return;
            changed = true;
            session.input({ type: "valueView", action: "pan", delta }, present ? "immediate" : "frame");
        } };
    },
    zoom(factor, cursorY) {
        const rect = surface.value?.getBoundingClientRect();
        if (!rect?.height || !frame.value?.valueNavigation || dragging.value || factor === 1) return false;
        session.input({ type: "valueView", action: "zoom", factor, y: (cursorY - rect.top) / rect.height }, present ? "immediate" : "frame");
        return true;
    },
    reset() {
        if (frame.value?.valueNavigation && !dragging.value) session.input({ type: "valueView", action: "reset" });
    },
};
watch(surface, (element, _, onCleanup) => {
    const lane = element?.closest(".lane");
    if (lane && navigation) onCleanup(navigation.registerValueNavigation(lane, valueNavigation));
});
function control(input: PresentationInput) {
    if (!error.value && (editable.value || (input.type === "control" && input.phase === "finish"))) session.input(input);
}
onBeforeUnmount(() => {
    disposed = true;
    settleCursorReply();
    unregisterInterrupt?.();
    clearTimeout(idleTimer);
    wake.cancel();
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    worker?.dispose();
    laneEdits.closeGestures();
});
</script>

<template>
    <NodeLane v-if="!program.manifest.timeline || time" :mode="mode" flush-content>
        <div ref="surface" class="plugin-surface" :data-status-hint="error || replacementError || editError || hoveredTarget?.hint || frame?.status"
            :class="{ 'plugin-seek': frame?.timelineGrid?.seek }"
            :data-status-shortcuts="hoveredTarget?.shortcuts ?? (frame?.timelineGrid?.seek ? 'Click/drag seek · Space ▶/Ⅱ' : undefined)"
            :style="{ cursor: hoveredTarget?.cursor }" :tabindex="frame?.itemEditing && editable ? -1 : undefined"
            :data-lane-item-editor="frame?.itemEditing && editable ? '' : undefined"
            @pointerdown.self="pointerDown" @pointermove.self="hover" @pointerleave="hovered = undefined"
            @contextmenu.self="openContext" @keydown="itemShortcut">
            <PresentationScene :marks="laneMarks" :highlighted-target="hoveredTarget?.id" />
            <LaneSettings v-if="frame?.panel && node.address && mode === 'editable' && !error" :label="frame.panel.label"
                dialog-label="Lane settings" :hint="editError || frame.status || frame.panel.label" :width="236" :preferred-height="260"
                @toggle="(open) => { panelOpen = open; if (open) menu.close(); }">
                <PresentationControls :rows="frame.panel.rows" :identity="addressKey(node.address)" :editable="editable" @input="control" />
            </LaneSettings>
            <span v-if="error" class="plugin-error" role="status">{{ error }}</span>
        </div>
    </NodeLane>
    <GenericPresentation v-else :mode="mode" />
    <Teleport to="body" v-if="menu.key.value !== null && node.address">
        <AnchoredPopover ref="contextPanel" class="plugin-context" :label="frame?.contextMenu?.label ?? 'Lane menu'" contextual
            :data-lane-key="addressKey(node.address)" data-lane-item-editor @escape="dismissContext" @toggle="contextToggled" @keydown="itemShortcut">
            <PresentationControls v-if="frame?.contextMenu" :rows="frame.contextMenu.rows" :identity="addressKey(node.address)"
                :editable="editable" contextual @input="control" />
        </AnchoredPopover>
    </Teleport>
</template>

<style scoped>
.plugin-surface { position: relative; width: 100%; height: 100%; overflow: hidden; touch-action: none; user-select: none; }
.plugin-seek { cursor: ew-resize; }
.plugin-context { width: min(214px, calc(100vw - 8px)); padding: 4px 8px; }
.plugin-error { position: absolute; top: 4px; left: 8px; color: var(--vscode-errorForeground); }
</style>

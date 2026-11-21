<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef, watch } from "vue";
import { addressKey } from "../../src/graphScope";
import { graphEditorShortcut, isTextEditingTarget } from "../../src/graphEditorShortcuts";
import { presentationNodeEvent } from "../../src/nodeEvents";
import type { NodeEventGesture } from "../../src/nodeEventMessaging";
import { PresentationEdits, type PresentationEdit } from "../../src/presentationEdits";
import type { PresentationFrame, PresentationGestureInput, PresentationInput, PresentationObservation, PresentationPoint,
    PresentationProgram, PresentationSavedState, PresentationSnapshot } from "../../src/presentationPlugin";
import { PresentationPluginSession } from "../../src/presentationPluginSession";
import { sampleTimeSpan, sampleWindow } from "../../src/sampleWaveform";
import { recordedEventTimestampsOperation } from "../../src/observations";
import { useOptionalGraphEditor } from "../graph/context";
import { visibleCursorDestination } from "../graph/cursorReturn";
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
import { usePresentationActive, usePortObservationViewport } from "./observationContext";
import { usePresentationFrame } from "./presentationFrames";
import { presentationDrag } from "./presentationDrag";
import { usePresentationSize } from "./presentationSize";
import { usePresentationState } from "./presentationStateContext";
import { PresentationPluginWorker } from "./presentationPluginWorker";

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
const editable = computed(() => props.mode === "editable" && !!editor?.scope.value?.editable
    && !!node.value.address && node.value.loadState === "ready");
const worker = "source" in props.program ? new PresentationPluginWorker(props.program.source) : undefined;
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
    restore: (saved) => { state.value = saved ?? null; },
});
const observed = (props.program.manifest.observations ?? []).map((need) => ({ id: need.id, view: usePortObservationViewport(() => {
    const { address, ports } = node.value;
    const timeline = time?.value;
    const event = need.operation === recordedEventTimestampsOperation;
    const port = ports.find((port) => port.direction === need.direction && port.name === need.port
        && port.kind === (event ? "event" : "continuous"));
    if (!address || !timeline || !port || (need.whenConnected && port.connectivity !== "connected")) return;
    const span = event ? sampleTimeSpan(timeline.beginSeconds, timeline.endSeconds, timeline.sampleRate) : undefined;
    const window = event ? span && { ...span, points: 0 }
        : sampleWindow(timeline.beginSeconds, timeline.endSeconds, timeline.sampleRate, need.points);
    if (window) return { source: { node: address, kind: need.direction, member: need.port, portKind: event ? "event" : undefined },
        operation: need.operation ?? "waveform", window,
        completeOnly: need.completeOnly };
}) }));
const observations = computed(() => Object.fromEntries(observed.map(({ id, view }): [string, PresentationObservation | null] => {
    const observation = view.value;
    if (!observation) return [id, null];
    const spans = observation.spans.map(({ begin, end, result }) => ({ begin: String(begin), end: String(end), result }));
    return [id, { ...observation, spans }];
})));
const gestures = new Map<string, NodeEventGesture>();
const acceptedRevision = ref(0);
let wakeTimer: ReturnType<typeof setTimeout> | undefined;
let wakeAt = Infinity;
let idleTimer: ReturnType<typeof setTimeout> | undefined;
let disposed = false;
let cursorReply: { input: PresentationGestureInput; resolve: (point: PresentationPoint | undefined) => void } | undefined;

function settleCursorReply(point?: PresentationPoint) {
    cursorReply?.resolve(point);
    cursorReply = undefined;
}

const edits = new PresentationEdits(sendEdit, () => { editsPending.value = edits.pending; }, (reason) => {
    editError.value = String(reason).slice(0, 512);
    cancelDrag?.();
    closeGestures();
});

async function sendEdit(command: PresentationEdit): Promise<void> {
    try {
        const acceptedRevision = await sendNodeEdit(command);
        if (command.requestId) session.input({ type: "editResult", requestId: command.requestId, acceptedRevision });
    } catch (reason) {
        if (!command.requestId) throw reason;
        const detail = reason instanceof Error ? reason.message : String(reason);
        session.input({ type: "editResult", requestId: command.requestId, error: detail.slice(0, 512) });
        if (command.type === "edit" && command.gesture) {
            const gesture = gestures.get(command.gesture);
            gestures.delete(command.gesture);
            void gesture?.finish().catch(() => {});
        }
    }
}

async function sendNodeEdit(command: PresentationEdit): Promise<number | undefined> {
    if (command.type === "finish") {
        const gesture = gestures.get(command.gesture);
        if (gesture) {
            gestures.delete(command.gesture);
            await gesture.finish();
            return gesture.acceptedRevision?.();
        }
        return;
    }
    if (!editable.value) throw new Error("The presentation node is no longer editable");
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
        return gesture.acceptedRevision?.();
    } finally { if (!key) await gesture.finish(); }
}

function closeGestures() {
    for (const gesture of gestures.values()) void gesture.finish().catch(() => {});
    gestures.clear();
}
function fail(reason: unknown) {
    if (disposed) return;
    error.value = String(reason).slice(0, 512);
    settleCursorReply();
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    closeGestures();
    worker?.stop();
}
function accept(next: PresentationFrame, draw: boolean, inputs: readonly PresentationInput[]) {
    if (cursorReply && inputs.includes(cursorReply.input)) settleCursorReply(draw ? next.drag?.cursor : undefined);
    const continuing = inputs.length > 0 || waking.value;
    const deferredState = !!present && inputs.length > 0
        && inputs.every((input) => input.type === "valueView" && input.action !== "reset");
    if (continuing) for (const command of next.commands ?? []) {
        if (command.type === "state") {
            if (!editor?.scope.value?.editable || !node.value.address) continue;
            state.value = command.value;
            save(!deferredState);
        } else if (command.type === "finish" || editable.value || command.requestId) edits.enqueue(command);
    }
    if (active.value && draw) frame.value = next;
    if (continuing && next.wakeAfterMs !== undefined) {
        const delay = Math.max(8, next.wakeAfterMs);
        const deadline = performance.now() + delay;
        if (deadline < wakeAt) {
            clearTimeout(wakeTimer);
            wakeAt = deadline;
            wakeTimer = setTimeout(() => {
                wakeAt = Infinity;
                session.input({ type: "wake" });
            }, delay);
        }
    }
    waking.value = wakeAt !== Infinity;
    if (next.cancelDrag) cancelDrag?.();
    if (dragging.value) editor?.refreshPointerHint();
    if (draw && menu.key.value !== null && !next.contextMenu) menu.close(true);
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
    return { node: { address: scopedNode.address, definitionId: scopedNode.definitionId, arguments: scopedNode.construction?.arguments ?? [],
        ports: scopedNode.ports.map((port) => ({ name: port.name, direction: port.direction, kind: port.kind,
            connected: port.connectivity === "connected", ...(port.kind === "continuous" && port.manualControl ? {
                manualValue: port.manualControl.liveValue ?? (port.manualControl.participates ? port.manualControl.value : undefined),
            } : {}) })) },
        revision, editable: editable.value, mode: props.mode, editsPending: editsPending.value || revision < acceptedRevision.value,
        editError: editError.value || undefined,
        viewport: { ...size.value, ...(timeline ? { time: { beginSeconds: timeline.beginSeconds,
            spanSeconds: timeline.spanSeconds ?? timeline.endSeconds - timeline.beginSeconds, sampleRate: timeline.sampleRate ?? undefined,
            gridMode: timeline.gridMode } } : {}) },
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
    let released: Promise<PresentationPoint | undefined> | undefined;
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
            const locate = (point: PresentationPoint | undefined) => {
                const element = surface.value;
                if (!point || !element?.isConnected) {
                    trace?.log("Presentation point has no visible destination", { point, connected: element?.isConnected ?? false });
                    return null;
                }
                const bounds = element.getBoundingClientRect();
                const requested = { x: bounds.left + point.x, y: bounds.top + point.y };
                const destination = visibleCursorDestination(element, requested);
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
    const discarded = edits.discardPending();
    for (const { requestId } of discarded) if (requestId)
        session.input({ type: "editResult", requestId, error: "Presentation edit interrupted" });
    session.input({ type: "interrupt", reason }, present ? "immediate" : "frame");
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
    clearTimeout(wakeTimer);
    session.dispose();
    edits.dispose();
    cancelDrag?.();
    worker?.stop();
    closeGestures();
});
</script>

<template>
    <NodeLane v-if="!program.manifest.timeline || time" :mode="mode" flush-content>
        <div ref="surface" class="plugin-surface" :data-status-hint="error || editError || hoveredTarget?.hint || frame?.status"
            :class="{ 'plugin-seek': frame?.seekMarks }"
            :data-status-shortcuts="hoveredTarget?.shortcuts ?? (frame?.seekMarks ? 'Click/drag seek · Space ▶/Ⅱ' : undefined)"
            :style="{ cursor: hoveredTarget?.cursor }" :tabindex="frame?.itemEditing && editable ? -1 : undefined"
            :data-lane-item-editor="frame?.itemEditing && editable ? '' : undefined"
            @pointerdown.self="pointerDown" @pointermove.self="hover" @pointerleave="hovered = undefined"
            @contextmenu.self="openContext" @keydown="itemShortcut">
            <PresentationScene :marks="frame?.marks" :highlighted-target="hoveredTarget?.id" />
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

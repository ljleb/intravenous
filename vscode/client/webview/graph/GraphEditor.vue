<script setup lang="ts">
import { provideObservations } from "../presentation/observationContext";
import { ClientInteractionRecorder, observationConfiguration } from "../../src/clientInteractionTrace";
import { PresentationObservations } from "../../src/presentationObservations";
import type { SerializedPresentationState } from "../../src/presentationStateJson";
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowReactive, shallowRef, watch } from "vue";
import type { GraphEditorHostMessage, GraphEditorState, GraphEditorWebviewMessage,
    GraphLaneInsertion, GraphPresentationPatch } from "../../src/graphEditorProtocol";
import { ObservationProfile } from "../../src/observationProfile";
import { GraphUiRecording } from "./graphUiRecording";
import { usePresentationFrame } from "../presentation/presentationFrames";
import { ObservationDeliverySequence } from "../../src/observationDelivery";
import { addressKey, graphScopeNodeLabel, isProjectInstance, newGraphLayout, presentationStateKey, readGraphViewState, type NodeAddress, type Point } from "../../src/graphScope";
import { isTimelineSnapNode } from "../../src/timelineSnap";
import { webviewApi } from "../host";
import PresentationHost from "../presentation/PresentationHost.vue";
import type { SemanticNode } from "../presentation/semanticNode";
import { provideGraphEditor, type KnobClipboard, type KnobDrag, type ObjectDrag } from "./context";
import { cursorWarpAnchor, PointerLockCursorReturn, type CursorWarpAnchor, type CursorTrace } from "./cursorReturn";
import NodeEditor from "./NodeEditor.vue";
import NodeDefinitionPicker from "./NodeDefinitionPicker.vue";
import type { NodeDefinitionPreview } from "../../src/nodeDefinitionSearch";
import { replaceProjectPresentations, selectPresentation } from "../presentation/presentations";
import { canContinuePortConnection, fixedConnectionPorts, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { beginPortConnection, canDropPortConnection, finishPortConnection, movePortConnection, type PortConnectionDrag } from "../../src/portConnectionGesture";
import { initialPortHoverShift, keyboardPortHoverShift, pointerPortHoverShift } from "../../src/portHoverShift";
import { connectionCursorOutputFraction, type PortColorColumns } from "../../src/connectionPreview";
import { ManualControlGestures } from "../../src/manualControlGesture";
import { provideManualControls } from "../presentation/manualControlContext";
import ManualValuePopover from "../presentation/ManualValuePopover.vue";
import { canEditManualControl, resolveManualControl } from "../../src/manualControls";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { KnobClicks } from "../../src/knobClicks";
import { PointerLockExitGuard } from "../../src/pointerLockExitGuard";
import { PointerLockExitRedirect } from "../../src/pointerLockExitRedirect";
import { TrustedHover } from "../../src/trustedHover";
import { providePlayback } from "../timeline/playbackContext";
import { useBeatGridEditing } from "../presentation/beatGridEditingContext";
import { useElementStatusHint } from "../statusHints";
import type { StatusHint } from "../../src/statusBarText";
import { projectHistoryShortcut } from "../../src/projectHistoryShortcut";
import { graphEditorShortcut, isTextEditingTarget } from "../../src/graphEditorShortcuts";
import { listenPortConnectionFocus } from "../../src/portConnectionFocus";
import { dropSelectedLanes, expandedGraphLanes, mergeLaneOrder, movableLaneSelection, moveLaneNesting, moveSelectedSiblingLanes,
    orderLanes, selectedProjectLaneIds, selectLaneRange, type LaneDestination } from "../../src/laneSelection";
import { connectionEndKey, connectionWirePath, LaneConnectionViewport, portEndpointKey, portSlotKey,
    resolveLaneConnectionEnds, selectedLaneConnections, selectedLaneWirePath,
    type PortWireAnchor, type ResolvedLaneConnections, type RevealedConnectionEnd, type SelectedLaneWire } from "../../src/selectedLaneConnections";

type HoveredPortGroup = { nodeKey: string; rowKey?: string; direction: "input" | "output" };
type PreviewAnchor = PortWireAnchor & { direction: "input" | "output" };
type WireOcclusions = { labels: DOMRect[]; knobs: DOMRect[] };

const wireOpacity = { faint: .19, emphasized: .72 };

const state = shallowRef<GraphEditorState>({ sessionStatus: "starting", scope: null,
    definitions: null, definitionUsage: {}, error: "" });
const playback = providePlayback();
const beats = useBeatGridEditing();
const elementStatus = useElementStatusHint();
onBeforeUnmount(() => playback.dispose());
const observations = new PresentationObservations((request) => webviewApi.postMessage(request), {
    initial: webviewApi.getState()?.meterModes,
    save: (meterModes) => webviewApi.setState({ ...webviewApi.getState(), meterModes }) }, { cell: shallowRef, map: shallowReactive });
provideObservations(observations);
let configuredObservations: string | undefined;
let clientTrace: ClientInteractionRecorder | undefined;
let clientTraceTimer: ReturnType<typeof setTimeout> | undefined;
const observationDelivery = new ObservationDeliverySequence();
let observationProfile: ObservationProfile | undefined;
let observationProfileTimer: ReturnType<typeof setTimeout> | undefined;
let graphUiRecording: GraphUiRecording | undefined;
let graphUiProfileTimer: ReturnType<typeof setTimeout> | undefined;
let playbackProfileMarker: { profileId: string; until: number } | undefined;
let pendingVueFlush: GraphUiRecording | undefined;
let graphVisible = true;
function profileGraphUi<T>(label: string, run: () => T): T {
    return graphUiRecording ? graphUiRecording.work.measure(label, run) : run();
}
function profileVueFlush(recording: GraphUiRecording) {
    if (pendingVueFlush === recording) return;
    pendingVueFlush = recording;
    const start = performance.now();
    void nextTick(() => {
        if (pendingVueFlush === recording) pendingVueFlush = undefined;
        if (recording === graphUiRecording) recording.work.record("Wait for Vue update after message", performance.now() - start);
    });
}
let armedCursorReturnTrace: string | undefined;
let cursorReturnTraceTimer: ReturnType<typeof setTimeout> | undefined;
function takeCursorReturnTrace(): CursorTrace | undefined {
    const id = armedCursorReturnTrace;
    if (!id) return;
    clearTimeout(cursorReturnTraceTimer);
    armedCursorReturnTrace = undefined;
    const start = performance.now();
    return { id, log: (step, details) => webviewApi.postMessage({
        type: "cursorReturnTrace", traceId: id, step, details: { ...details, elapsedMs: Math.round(performance.now() - start) },
    } satisfies GraphEditorWebviewMessage) };
}
const layout = ref(newGraphLayout());
const manualVersion = ref(0);
const timelineZoomHint = ref<string | null>(null);
let lastInteractionHint: StatusHint | null | undefined;
let pointerLockError = "";
let suppressDragContextMenu = false;
let objectDrag: (ObjectDrag & {
    pointerId: number; button: number; buttonMask: number; anchor: CursorWarpAnchor | null; moved: boolean;
}) | null = null;
function selectLaneByKey(key: string, modifiers?: { shiftKey?: boolean; ctrlKey?: boolean; metaKey?: boolean }) {
    const order = laneKeys.value;
    if (!order.includes(key)) return;
    const selection = selectLaneRange(selectedLanes.value, order, laneSelectionAnchor, key, modifiers);
    selectedLanes.value = selection.selected;
    laneSelectionAnchor = selection.anchor;
}
const moduleFooterSeamHeight = 6;
function moduleFooterWithin(row: Element, point: Point, end: number): HTMLButtonElement | null {
    if (point.y >= end) return null;
    const footer = row.querySelector<HTMLButtonElement>(".lane-label > .module-footer");
    if (!footer) return null;
    const bounds = footer.getBoundingClientRect();
    const start = Math.max(bounds.top, bounds.bottom - moduleFooterSeamHeight);
    if (point.x >= bounds.left && point.x < bounds.right && point.y >= start) return footer;
    const chevron = footer.querySelector(".module-footer-chevron")?.getBoundingClientRect();
    return chevron && point.x >= chevron.left && point.x < chevron.right
        && point.y >= chevron.top && point.y < chevron.bottom ? footer : null;
}
function moduleFooterAcrossSeam(target: EventTarget | null, point: Point) {
    if (!(target instanceof Element) || target.closest("button, a[href], input, [data-port-control]")) return null;
    const row = target.closest<HTMLElement>(".lane[data-lane-row]");
    if (!row) return null;
    const header = target.closest<HTMLElement>(".lane-label");
    if (!header && target.closest(".lane-content")) return null;
    const bounds = row.getBoundingClientRect();
    const cursorTarget = header ?? row;
    // Fractional lane positions can leave the boundary event on either row.
    const ownFooter = moduleFooterWithin(row, point, bounds.bottom + moduleFooterSeamHeight);
    if (ownFooter) return { footer: ownFooter, cursorTarget };

    const previous = row.previousElementSibling;
    if (!previous?.matches(".lane[data-lane-row]")) return null;
    const end = bounds.top + Math.min(moduleFooterSeamHeight, bounds.height / 3);
    const footer = moduleFooterWithin(previous, point, end);
    return footer ? { footer, cursorTarget } : null;
}
function pressedModuleFooterAcrossSeam(event: MouseEvent) {
    if (event.button !== 0 || event.ctrlKey || event.shiftKey || event.altKey || event.metaKey) return null;
    return moduleFooterAcrossSeam(event.target, { x: event.clientX, y: event.clientY });
}
function clickModuleFooterAcrossSeam(event: MouseEvent) {
    if (event.defaultPrevented) return;
    const target = pressedModuleFooterAcrossSeam(event);
    if (!target) return;
    event.stopPropagation();
    target.footer.click();
}
function selectLaneAtPointer(event: PointerEvent) {
    if (event.button !== 0 && event.button !== 2) return;
    if (!(event.target instanceof Element)) return;
    const target = pressedModuleFooterAcrossSeam(event)?.footer ?? event.target;
    const key = target.closest<HTMLElement>("[data-lane-key]")?.dataset.laneKey;
    if (!key) return;
    if (event.button === 0 && event.shiftKey && !event.ctrlKey && !event.metaKey && !event.altKey
        && state.value.scope?.editable && selectedLanes.value.has(key)
        && event.target.closest(".lane-label") && !event.target.closest(".node-ports, input, .module-footer, .copy-debug")
        && projectLanes.value.some(({ address }) => addressKey(address) === key)
        && startLaneDrag(event, key)) return;
    selectLaneByKey(key, event);
}
const knobClicks = new KnobClicks();
const activeKnob = ref<string | null>(null);
let knobDrag: (KnobDrag & { pointerId: number }) | null = null;
const valueEntryAnchor = shallowRef<HTMLElement>();
const knobClipboards = new WeakMap<Element, KnobClipboard>();
const pendingKnobPastes = new Map<number, (text: string) => string | undefined>();
let knobClipboardRequest = 0;
let knobClipboardStatus: StatusHint | null = null;
let knobClipboardStatusTimer: ReturnType<typeof setTimeout> | undefined;

const manualGestures = new ManualControlGestures(
    (action) => {
        clientTrace?.record({ type: "manual", action });
        webviewApi.postMessage({ type: "manualControlAction", action });
    },
    () => crypto.randomUUID(), () => { ++manualVersion.value; publishInteractionHint(); }, {
        initial: webviewApi.getState()?.outputInversions,
        save: (outputInversions) => webviewApi.setState({ ...webviewApi.getState(), outputInversions }),
    });
const manualValueEntry = computed(() => { void manualVersion.value; return manualGestures.valueEditing.view(); });
const pointerLock = new KnobPointerLock<Point>({
    request: () => surface.value!.requestPointerLock(),
    exit: () => document.exitPointerLock(),
    locked: () => !!surface.value && document.pointerLockElement === surface.value,
}, (id, delta) => {
    if (objectDrag?.pointerId === id) {
        if (!objectDrag.moved && (delta.x || delta.y)) {
            objectDrag.moved = true;
            objectDrag.trace?.log("First relative pointer movement", delta);
        }
        objectDrag.moveBy(delta);
        if (objectDrag?.hint) publishInteractionHint();
    } else {
        knobClicks.move(delta.x, delta.y);
        knobDrag?.moveBy(delta);
        publishInteractionHint();
    }
}, (id, point, lockOrigin) => {
    if (objectDrag?.pointerId === id) {
        const drag = objectDrag;
        objectDrag = null;
        document.documentElement.removeAttribute("data-hidden-drag-cursor");
        const target = drag.finish(point, lockOrigin);
        const follow = drag.moved && drag.anchor && target ? { ...drag.anchor, ...target } : undefined;
        drag.trace?.log("Object drag finished", { moved: drag.moved, mouse: !!drag.anchor, targetProvided: !!target });
        cursorReturn.finish(point, lockOrigin, follow, pointerLockError, drag.trace);
        publishInteractionHint();
    } else {
        cursorReturn.finish(point, lockOrigin);
        if (!point) knobClicks.cancel();
        const drag = knobDrag;
        knobDrag = null;
        activeKnob.value = null;
        drag?.finish(point);
        publishInteractionHint();
    }
    if (surface.value?.hasPointerCapture(id)) surface.value.releasePointerCapture(id);
}, (message) => {
    objectDrag?.trace?.log("Pointer lock failed", { message });
    pointerLockError = message; publishInteractionHint();
});
watch(beats.version, () => {
    // Graph replacement, undo, or a failed edit can end the gesture before mouseup.
    if (knobDrag?.active && !knobDrag.active()) pointerLock.cancel();
}, { flush: "sync" });
function capturePointerControl(event: PointerEvent, element: HTMLElement, origin: Point, lock: boolean) {
    event.preventDefault(); event.stopPropagation();
    pointerLockError = "";
    element.setPointerCapture(event.pointerId);
    pointerLock.start(event.pointerId, origin, lock && event.pointerType === "mouse");
}
function canStartObjectDrag(element: HTMLElement | null): element is HTMLElement {
    return !!element && !objectDrag && !knobDrag && !connectionDrag.value
        && (!document.pointerLockElement || document.pointerLockElement === element);
}
function startObjectDrag(event: PointerEvent, drag: ObjectDrag) {
    const element = surface.value;
    const buttonMask = [1, 4, 2, 8, 16][event.button];
    if (!buttonMask || !canStartObjectDrag(element)) {
        drag.trace?.log("Pointer session refused", { surface: !!element, button: event.button, objectDrag: !!objectDrag,
            knobDrag: !!knobDrag, connectionDrag: !!connectionDrag.value, lockHeld: !!document.pointerLockElement });
        return;
    }
    if (drag.begin?.() === false) return;
    const session = { ...drag, pointerId: event.pointerId, button: event.button, buttonMask,
        anchor: cursorWarpAnchor(event, window.devicePixelRatio), moved: false };
    objectDrag = session;
    suppressDragContextMenu = event.button === 2;
    if (drag.hideCursor && event.pointerType === "mouse") document.documentElement.setAttribute("data-hidden-drag-cursor", "");
    capturePointerControl(event, element, { x: event.clientX, y: event.clientY }, drag.lock !== false);
    publishInteractionHint();
    drag.trace?.log("Pointer session started", { ...pointerLock.status(), capture: element.hasPointerCapture(event.pointerId) });
    return () => { if (objectDrag === session) pointerLock.cancel(); };
}
provideManualControls({ gestures: manualGestures, version: manualVersion, setEnabled: (control, enabled) => {
    webviewApi.postMessage({ type: "setManualControlEnabled", control, enabled });
} });
function startKnob(event: PointerEvent, key: string, begin: () => KnobDrag | undefined, reset?: () => void) {
    const element = surface.value;
    if (!element || knobDrag || objectDrag) return;
    const control = event.currentTarget as HTMLElement;
    if (!control.closest(":popover-open, [role=dialog]")) element.focus({ preventScroll: true });
    else control.focus({ preventScroll: true });
    const origin = { x: event.clientX, y: event.clientY };
    if (event.pointerType === "mouse" && knobClicks.press(key, origin, performance.now(), event.detail) && reset) {
        reset();
        return;
    }
    const drag = begin();
    if (!drag) { knobClicks.cancel(); return; }
    knobDrag = { ...drag, pointerId: event.pointerId };
    activeKnob.value = key;
    element.setPointerCapture(event.pointerId);
    pointerLockError = "";
    pointerLock.start(event.pointerId, origin, event.pointerType === "mouse");
    publishInteractionHint();
}
function publishInteractionHint() {
    const manual = document.hasFocus() ? manualGestures.hint() : null;
    const adjustingKnob = manualGestures.pointerControl();
    const edit = document.hasFocus() ? objectDrag?.hint?.() : undefined;
    const drag = playback.dragPosition.value;
    const hint: StatusHint | null = (manual && (adjustingKnob || manualGestures.valueEditing.view())
        ? { ...manual, text: manual.text + (adjustingKnob && pointerLockError ? ` · Pointer lock unavailable. ${pointerLockError}` : "") } : null)
        ?? (document.hasFocus() ? knobDrag?.hint?.() : undefined)
        ?? (edit ? { ...edit, text: edit.text + (pointerLockError ? " · Pointer lock unavailable" : "") } : null)
        ?? (timelineZoomHint.value ? { text: timelineZoomHint.value } : null)
        ?? (drag === null ? null : { text: `Start ${drag} smp${pointerLockError ? " · Pointer lock unavailable" : ""}` })
        ?? knobClipboardStatus ?? elementStatus.hint.value ?? manual ?? null;
    if (lastInteractionHint !== undefined && hint?.text === lastInteractionHint?.text
        && hint?.shortcuts === lastInteractionHint?.shortcuts) return;
    lastInteractionHint = hint;
    webviewApi.postMessage({ type: "graphInteractionHint", hint });
}
watch([elementStatus.hint, playback.dragPosition, timelineZoomHint], publishInteractionHint);
function focusPointerHint() {
    if (document.hasFocus()) pointerLock.retryAfterFocus();
    lastInteractionHint = undefined; publishInteractionHint();
}
function moveCapturedControl(event: PointerEvent) {
    if (objectDrag?.pointerId === event.pointerId) {
        event.preventDefault(); event.stopImmediatePropagation();
        if (document.pointerLockElement === surface.value) return;
        objectDrag.modifiers?.(event);
        if (!(event.buttons & objectDrag.buttonMask)) { releaseCapturedControl(event); return; }
        const origin = pointerLock.status().origin;
        if (!objectDrag.moved && (event.clientX !== origin?.x || event.clientY !== origin?.y)) {
            objectDrag.moved = true;
            objectDrag.trace?.log("First captured pointer movement", { x: event.clientX, y: event.clientY });
        }
        objectDrag.move({ x: event.clientX, y: event.clientY });
        if (objectDrag?.hint) publishInteractionHint();
        return;
    }
    if (knobDrag?.pointerId !== event.pointerId) return;
    event.stopImmediatePropagation();
    if (document.pointerLockElement === surface.value) return;
    if (!(event.buttons & 1)) { releaseCapturedControl(event); return; }
    knobClicks.move(event.movementX, event.movementY);
    knobDrag.move({ x: event.clientX, y: event.clientY });
    publishInteractionHint();
}
function releaseCapturedControl(event: PointerEvent) {
    const drag = objectDrag?.pointerId === event.pointerId ? objectDrag : null;
    if (event.type === "pointerup" && event.button !== (drag?.button ?? 0)) return;
    if (drag) {
        event.stopImmediatePropagation();
        drag.trace?.log("Pointer release event", { type: event.type, x: event.clientX, y: event.clientY, buttons: event.buttons });
        if (event.type !== "pointerup" && event.type !== "pointermove") { pointerLock.cancel(); return; }
        drag.modifiers?.(event);
    }
    if (knobDrag?.pointerId === event.pointerId) knobClicks.release(performance.now());
    pointerLock.release(event.pointerId, { x: event.clientX, y: event.clientY });
}
function lockedControlMove(event: MouseEvent) {
    if (document.pointerLockElement !== surface.value) return;
    event.stopImmediatePropagation();
    objectDrag?.modifiers?.(event);
    // A synthesized move with an empty buttons mask is not a mouse release.
    // Mouseup, pointerup, native focus loss and pointerlockchange own termination.
    pointerLock.move({ x: event.movementX, y: event.movementY });
}
function lockedControlRelease(event: MouseEvent) {
    if (event.button !== (objectDrag?.button ?? 0)) return;
    if (objectDrag && document.pointerLockElement !== surface.value) return;
    const id = objectDrag?.pointerId ?? knobDrag?.pointerId;
    if (id === undefined) return;
    objectDrag?.trace?.log("Mouse release event", { type: event.type, x: event.clientX, y: event.clientY, buttons: event.buttons });
    objectDrag?.modifiers?.(event);
    if (knobDrag) knobClicks.release(performance.now());
    pointerLock.release(id, { x: event.clientX, y: event.clientY });
}
function controlLockChanged() {
    objectDrag?.trace?.log("Browser reported a pointer lock change", { locked: document.pointerLockElement === surface.value });
    pointerLock.changed();
    cursorReturn.unlocked();
    if (pointerLock.status().granted && document.pointerLockElement === surface.value) {
        // Capture is only a fallback while the lock is pending. A successful
        // lock owns relative mouse input and must not retain a capture target.
        const id = objectDrag?.pointerId ?? knobDrag?.pointerId;
        if (id !== undefined && surface.value?.hasPointerCapture(id)) surface.value.releasePointerCapture(id);
        pointerLockError = "";
        publishInteractionHint();
    }
}
function controlLockFailed() { pointerLock.error(); }
function wheelControl(event: WheelEvent) {
    const delta = event.deltaY || event.deltaX;
    if (knobDrag) knobDrag.changeSensitivity(delta);
    else if (!objectDrag?.wheel?.(delta)) return;
    event.preventDefault(); event.stopImmediatePropagation();
    publishInteractionHint();
}
function clickOutsideManual(event: PointerEvent) {
    if (!objectDrag) suppressDragContextMenu = false;
    cursorReturn.cancel("A new pointer press arrived.");
    const target = event.target instanceof Element ? event.target : null;
    if (target?.closest(".manual-value-popover")) return;
    manualGestures.cancelEntry();
    if (!target?.closest(".manual-input, .knob-control") || event.button !== 0) knobClicks.cancel();
    if (!target?.closest(".manual-input, .knob-control") && !objectDrag) { pointerLock.cancel(); manualGestures.finish(); }
}
function guardDragContextMenu(event: MouseEvent) {
    if (suppressDragContextMenu && event.button === 2) {
        event.preventDefault();
        event.stopImmediatePropagation();
    }
}
function blurPointerControls() {
    cursorReturn.cancel("The editor released its pointer controls.");
    knobClicks.cancel(); pointerLock.cancel(); manualGestures.finish(); manualGestures.cancelEntry(); manualGestures.hover(null);
    // Positions after focus returns come from real movement. The cleared knob
    // hover must be entered again by the next move, even within the same knob.
    pointerLockExit.end();
    trustedHover.reset();
}

// Until the first real movement after a pointer lock ends, the host
// reports a displaced position while the cursor is visibly at the lock origin.
// Moves and boundary events at that position are dropped, and presses, releases
// and clicks are delivered at the origin. Hover never follows the displaced
// position because it comes only from TrustedHover, registered after this guard.
const pointerLockExit = new PointerLockExitGuard();
const cursorReturn = new PointerLockCursorReturn(pointerLockExit, () => !!document.pointerLockElement,
    (run) => { void nextTick(run); }, (message) => webviewApi.postMessage(message));
const pointerLockExitTypes = [
    "pointermove", "mousemove", "pointerover", "pointerout", "pointerenter", "pointerleave",
    "mouseover", "mouseout", "mouseenter", "mouseleave",
    "pointerdown", "pointerup", "mousedown", "mouseup", "click", "dblclick", "auxclick", "contextmenu",
];
const pointerLockExitRedirect = new PointerLockExitRedirect(pointerLockExit, {
    locked: () => !!document.pointerLockElement,
    elementAt: (point) => document.elementFromPoint(point.x, point.y),
    copy: (event, init) => event instanceof PointerEvent
        ? new PointerEvent(event.type, { ...init, view: window }) : new MouseEvent(event.type, { ...init, view: window }),
});
function guardPointerLockExit(event: Event) {
    if (event instanceof MouseEvent) pointerLockExitRedirect.handle(event);
}
// Registered before the component's other window listeners, including those
// added later by port popovers, so no capture handler sees a displaced event.
for (const type of pointerLockExitTypes) window.addEventListener(type, guardPointerLockExit, true);
let hoveredModuleFooterAcrossSeam: ReturnType<typeof moduleFooterAcrossSeam> = null;
const trustedHover = new TrustedHover(undefined, (target, pointerMoved, point) => {
    const hovered = target instanceof Element && point ? moduleFooterAcrossSeam(target, point) : null;
    if (hovered?.footer !== hoveredModuleFooterAcrossSeam?.footer
        || hovered?.cursorTarget !== hoveredModuleFooterAcrossSeam?.cursorTarget) {
        hoveredModuleFooterAcrossSeam?.footer.removeAttribute("data-seam-hovered");
        hoveredModuleFooterAcrossSeam?.cursorTarget.removeAttribute("data-module-footer-target");
        hovered?.footer.setAttribute("data-seam-hovered", "");
        hovered?.cursorTarget.setAttribute("data-module-footer-target", "");
        hoveredModuleFooterAcrossSeam = hovered;
    }
    elementStatus.hover(hovered?.footer ?? target, pointerMoved);
});

const surface = ref<HTMLElement | null>(null);
const connectionDrag = shallowRef<PortConnectionDrag | null>(null);
const hoverShift = shallowRef(initialPortHoverShift());
const connectionStartAnchor = shallowRef<PreviewAnchor | null>(null);
const dropPreviewAnchor = shallowRef<PreviewAnchor | null>(null);
const hoveredPortGroup = shallowRef<HoveredPortGroup | null>(null);
const previewColorColumns = shallowRef<PortColorColumns[]>([]);
let sourceGroupToLeave: HoveredPortGroup | null = null;
const selectedLanes = shallowRef<ReadonlySet<string>>(new Set());
const expandedModules = shallowRef<ReadonlyMap<string, NodeAddress>>(new Map());
const lanes = computed(() => profileGraphUi("Build lane list", () => {
    const scope = state.value.scope;
    return scope ? expandedGraphLanes(scope, layout.value.laneOrder, expandedModules.value, layout.value.laneParents)
        .filter(({ node }) => !isTimelineSnapNode(node)) : [];
}));
const lanesByRow = computed(() => new Map(lanes.value.map((lane) => [lane.rowKey, lane])));
const visibleScope = computed(() => {
    const scope = state.value.scope;
    if (!scope) return null;
    const children = new Map(lanes.value.filter(({ depth, node }) => depth > 0 && !isProjectInstance(node.address))
        .map(({ node }) => [addressKey(node.address), node]));
    return children.size ? { ...scope, nodes: [...scope.nodes, ...children.values()] } : scope;
});
const wireConnections = computed(() => visibleScope.value ? resolveLaneConnectionEnds(visibleScope.value) : []);
const wireConnectionsById = computed(() => new Map(wireConnections.value.map((entry) => [entry.id, entry])));
let wireLaneLayout: {
    container: HTMLElement;
    lanes: typeof lanes.value;
    connections: ResolvedLaneConnections;
    rows: HTMLElement[];
    byAddress: Map<string, HTMLElement[]>;
    index: LaneConnectionViewport;
} | undefined;
function readWireLaneLayout(container: HTMLElement, connections: ResolvedLaneConnections) {
    const currentLanes = lanes.value;
    if (wireLaneLayout?.container === container && wireLaneLayout.lanes === currentLanes
        && wireLaneLayout.connections === connections) return wireLaneLayout;
    const rows = [...container.querySelectorAll<HTMLElement>(":scope > [data-lane-address]")];
    const byAddress = new Map<string, HTMLElement[]>();
    const addresses = rows.map((row) => {
        const address = row.dataset.laneAddress!;
        const group = byAddress.get(address);
        if (group) group.push(row);
        else byAddress.set(address, [row]);
        return address;
    });
    return wireLaneLayout = { container, lanes: currentLanes, connections, rows, byAddress,
        index: new LaneConnectionViewport(connections, addresses) };
}
const observationLaneNames = computed(() => new Map(visibleScope.value?.nodes.map((node) => [addressKey(node.address),
    graphScopeNodeLabel(node)])));
const observationLaneCopies = computed(() => {
    const counts = new Map<string, number>();
    for (const { node } of lanes.value) {
        const key = addressKey(node.address);
        counts.set(key, (counts.get(key) ?? 0) + 1);
    }
    return counts;
});
function observationLaneName(address: NodeAddress): string {
    const key = addressKey(address);
    const name = observationLaneNames.value.get(key);
    const identity = address.path?.length ? key : address.instanceId.slice(0, 8);
    const copies = observationLaneCopies.value.get(key) ?? 0;
    const label = name ? `${name} [${identity}]` : identity;
    return copies > 1 ? `${label} (${copies} lane rows)` : label;
}
const laneKeys = computed(() => lanes.value.map(({ node }) => addressKey(node.address)));
const projectLanes = computed(() => lanes.value.filter(({ node }) => isProjectInstance(node.address)).map(({ node }) => node));
const selectedLaneIds = computed(() => selectedProjectLaneIds(lanes.value, selectedLanes.value));

function toggleModule(address: NodeAddress) {
    const expanded = new Map(expandedModules.value);
    const key = addressKey(address);
    if (expanded.delete(key)) {
        const scope = state.value.scope;
        if (scope) {
            const remaining = expandedGraphLanes(scope, layout.value.laneOrder, expanded, layout.value.laneParents);
            const keys = new Set(remaining.map(({ node }) => addressKey(node.address)));
            for (const candidate of expanded.keys())
                if (!keys.has(candidate)) expanded.delete(candidate);
        }
    } else expanded.set(key, address);
    applyExpandedModules(expanded);
}
function applyExpandedModules(expanded: ReadonlyMap<string, NodeAddress>) {
    expandedModules.value = expanded;
    webviewApi.postMessage({ type: "setExpandedModules", modules: [...expanded.values()] } satisfies GraphEditorWebviewMessage);
}
function showLaneParents(parents: Readonly<Record<string, string>>, moved: ReadonlySet<string>) {
    const expanded = new Map(expandedModules.value);
    const destinations = new Set([...moved].map((child) => parents[child]));
    for (const lane of lanes.value) {
        const address = lane.node.membershipAddress ?? lane.node.address;
        const key = addressKey(lane.node.address);
        if (!expanded.has(key) && destinations.has(addressKey(address))) {
            expanded.set(key, lane.node.address);
        }
    }
    if (expanded.size !== expandedModules.value.size) applyExpandedModules(expanded);
}
function currentProjectLaneOrder() {
    const nodes = state.value.scope?.nodes.filter((node) => !isTimelineSnapNode(node)) ?? [];
    return mergeLaneOrder(layout.value.laneOrder, orderLanes(nodes, layout.value.laneOrder).map(({ address }) => addressKey(address)));
}
function startLaneDrag(event: PointerEvent, key: string) {
    const selected = new Set(selectedLanes.value);
    const roots = movableLaneSelection(lanes.value, selected);
    const origin = { x: event.clientX, y: event.clientY };
    let moved = false;
    let marked: HTMLElement | undefined;
    const destinationAt = (point: Point): { destination: LaneDestination; row: HTMLElement; placement: string } | undefined => {
        const row = document.elementFromPoint(point.x, point.y)?.closest<HTMLElement>("[data-lane-row]");
        if (!row || !surface.value?.contains(row)) return;
        const target = lanesByRow.value.get(row.dataset.laneRow!);
        if (!target || roots.some(({ rowKey }) => target.rowKey === rowKey || target.rowKey.startsWith(rowKey + "/"))) return;
        const address = addressKey(target.node.address);
        if (selected.has(address)) return;
        const rect = row.getBoundingClientRect();
        const fraction = (point.y - rect.top) / rect.height;
        if (target.node.isModule && fraction >= .25 && fraction <= .75)
            return { destination: { parent: target }, row, placement: "inside" };
        if (!isProjectInstance(target.node.address)) return;
        const parent = target.parentRowKey === undefined ? undefined : lanesByRow.value.get(target.parentRowKey);
        return fraction < .5
            ? { destination: { parent, before: address }, row, placement: "before" }
            : { destination: { parent, after: address }, row, placement: "after" };
    };
    const update = (point: Point) => {
        moved ||= Math.hypot(point.x - origin.x, point.y - origin.y) >= 5;
        if (!moved) return;
        marked?.removeAttribute("data-lane-drop");
        const target = destinationAt(point);
        marked = target?.row;
        target?.row.setAttribute("data-lane-drop", target.placement);
    };
    const started = startObjectDrag(event, {
        lock: false,
        move: update,
        moveBy: () => {},
        finish(point): undefined {
            marked?.removeAttribute("data-lane-drop");
            if (!point) return;
            if (!moved) { selectLaneByKey(key, event); return; }
            const target = destinationAt(point);
            if (!target) return;
            const change = dropSelectedLanes(lanes.value, currentProjectLaneOrder(), layout.value.laneParents, selected, target.destination);
            if (!change) return;
            showLaneParents(change.parents, selected);
            save(change.order, change.parents);
        },
    });
    if (started) {
        surface.value?.focus({ preventScroll: true });
        event.preventDefault();
        event.stopImmediatePropagation();
    }
    return !!started;
}
let laneSelectionAnchor: string | null = null;
let pendingLaneCopies: string | undefined;
let insertedSelection: string[] = [];
const selectedWires = shallowRef<SelectedLaneWire[]>([]);
const movingWireId = computed(() => connectionDrag.value?.edit?.connection.id);
const sourceWires = computed(() => selectedWires.value.filter((wire) => wire.sourceOwned));
const wireLayers = computed(() => {
    const layers: Record<keyof typeof wireOpacity, { wire: SelectedLaneWire; index: number }[]> = { faint: [], emphasized: [] };
    const moving = movingWireId.value;
    selectedWires.value.forEach((wire, index) => {
        if (wire.sourceOwned) return;
        const layer = wire.emphasized && (moving === undefined || !wire.connectionIds.includes(moving)) ? "emphasized" : "faint";
        layers[layer].push({ wire, index });
    });
    return layers;
});
const dragAnchors = shallowRef<PreviewAnchor[]>([]);
const wireClips = shallowRef<Array<{ x: number; y: number; width: number; height: number }>>([]);
const wireOcclusions = shallowRef<WireOcclusions>({ labels: [], knobs: [] });
let pendingWireUpdate = false;
let disposed = false;
let wireResizeObserver: ResizeObserver | null = null;
let stopListeningForFocus: (() => void) | undefined;
const editing = ref<string | null>(null);
const pickerOpen = ref(false);
const pickerReply = shallowRef<Extract<GraphEditorHostMessage, { type: "nodeDefinitionSearchResult" }>>();
const pickerCreationError = ref("");
const creatingPackage = ref(false);
const packageName = ref("");
const editedNode = computed(() => state.value.scope?.nodes.find((entry) => entry.instance.instanceId === editing.value));
const root = computed<SemanticNode>(() => ({
    id: "", instanceId: "", definitionId: "", kind: "Project graph", sourceIdentity: "",
    memberCount: 0, ports: [], members: [], sourceSpans: [], scope: visibleScope.value ?? undefined,
}));
const cursorOutputFraction = computed(() => connectionDrag.value
    ? connectionCursorOutputFraction(connectionDrag.value.end, previewColorColumns.value) : 0.5);
function wireColor(direction: "input" | "output"): string {
    return `var(--iv-port-${direction})`;
}
const cursorWireColor = computed(() =>
    `color-mix(in srgb, ${wireColor("input")} ${(1 - cursorOutputFraction.value) * 100}%, ${wireColor("output")} ${cursorOutputFraction.value * 100}%)`);
const previewWires = computed(() => {
    const drag = connectionDrag.value;
    if (!drag?.moved) return [];
    const anchors = drag.edit ? dragAnchors.value : connectionStartAnchor.value ? [connectionStartAnchor.value] : [];
    return anchors.map((anchor) => ({ anchor, cursor: drag.end,
        path: connectionWirePath(anchor, drag.end),
    }));
});
const dropPreviewWires = computed(() => {
    const target = dropPreviewAnchor.value;
    return target ? previewWires.value.map((wire) => ({ ...wire, target,
        path: connectionWirePath(wire.anchor, target) })) : [];
});
const changedPresentationStates = shallowReactive(new Map<string, SerializedPresentationState>());
const pendingPresentation = shallowReactive(new Map<number, { patch: GraphPresentationPatch; revision?: number }>());
const laneViewVersions = computed(() => new Map((state.value.scope?.presentation?.laneViews ?? []).map(
    ({ address, presentationId, version }) => [presentationStateKey(address, presentationId), version])));
const laneViewStates = shallowReactive(new Map<string, string>());
const laneViewReads = shallowReactive(new Map<string, number>());
let nextPresentationRequest = 0;
let nextLaneViewRead = 0;

function requestPresentationState(key: string): SerializedPresentationState | undefined {
    const pending = changedPresentationStates.get(key);
    if (pending !== undefined) return pending;
    let submitted: SerializedPresentationState | undefined;
    for (const { patch } of pendingPresentation.values()) {
        if (Object.hasOwn(patch.presentationStates, key)) submitted = patch.presentationStates[key];
    }
    if (submitted !== undefined) return submitted;

    const version = laneViewVersions.value.get(key);
    if (version === undefined) return null;
    const saved = laneViewStates.get(version);
    if (saved === undefined && !laneViewReads.has(version)) {
        const requestId = ++nextLaneViewRead;
        laneViewReads.set(version, requestId);
        webviewApi.postMessage({ type: "readLaneView", requestId, version } satisfies GraphEditorWebviewMessage);
    }
    return saved;
}

function updateProjectPresentation() {
    const scope = state.value.scope;
    const view = readGraphViewState(scope?.presentation);
    for (const [id, { patch, revision }] of pendingPresentation) {
        if (revision !== undefined && (scope?.sessionRevision ?? -1) >= revision) {
            pendingPresentation.delete(id);
            continue;
        }
        if (patch.laneOrder) view.laneOrder = patch.laneOrder;
        if (patch.laneParents) view.laneParents = patch.laneParents;
    }
    const currentOrder = layout.value.laneOrder;
    if (currentOrder.length !== view.laneOrder.length || currentOrder.some((key, index) => key !== view.laneOrder[index]))
        layout.value.laneOrder = view.laneOrder;
    const currentParents = layout.value.laneParents;
    if (Object.keys(currentParents).length !== Object.keys(view.laneParents).length
        || Object.entries(view.laneParents).some(([key, parent]) => currentParents[key] !== parent))
        layout.value.laneParents = view.laneParents;
}
function save(laneOrder?: readonly string[], laneParents?: Record<string, string>) {
    if (!laneOrder && !laneParents && !changedPresentationStates.size) return;
    const presentationStates = Object.fromEntries(changedPresentationStates);
    changedPresentationStates.clear();
    const patch = { presentationStates, laneOrder: laneOrder && [...laneOrder], laneParents };
    const requestId = ++nextPresentationRequest;
    pendingPresentation.set(requestId, { patch });
    updateProjectPresentation();
    webviewApi.postMessage({ type: "saveGraphPresentation", requestId, ...patch });
}

function selectInsertedLanes() {
    if (!insertedSelection.length) return;
    const visible = new Set(laneKeys.value);
    if (!insertedSelection.every((key) => visible.has(key))) return;
    selectedLanes.value = new Set(insertedSelection);
    laneSelectionAnchor = insertedSelection.at(-1)!;
    insertedSelection = [];
}

function selectNewLanes(lanes: readonly GraphLaneInsertion[]) {
    insertedSelection = lanes.map(({ instanceId }) => addressKey({ instanceId }));
    selectInsertedLanes();
}
const presentationInterrupts = new Set<(reason: "history" | "disconnected") => void>();
const projectHistoryRequest = ref<number>();
let finishedProjectHistoryRequest = 0;
provideGraphEditor({ layout, save, profileGraphUi, historyPending: computed(() => projectHistoryRequest.value !== undefined),
    requestPresentationState,
    presentationStateChanged: (key, state) => changedPresentationStates.set(key, state),
    finishPointerLock: (release, lockOrigin, follow, error) => cursorReturn.finish(release, lockOrigin, follow, error),
    startObjectDrag, takeCursorReturnTrace,
    refreshPointerHint: publishInteractionHint,
    registerPresentationInterrupt(interrupt) {
        presentationInterrupts.add(interrupt);
        return () => { presentationInterrupts.delete(interrupt); };
    },
    setTimelineZoomHint: (hint) => { timelineZoomHint.value = hint; },
    copyNodeDebugDetails: (address) => webviewApi.postMessage({ type: "copyNodeDebugDetails", nodeKey: addressKey(address) }),
    scope: visibleScope, lanes, expandedModules, toggleModule,
    shiftHeld: computed(() => hoverShift.value.held),
    connectionDrag, hoveredPortGroup, startConnection, cancelConnection, activeKnob, startKnob,
    cancelKnob() {
        if (!knobDrag) return false;
        knobClicks.cancel();
        pointerLock.cancel();
        return true;
    },
    openKnobValue(anchor, label, value, limits, apply, bindValueEntry) {
        if (knobDrag) pointerLock.cancel();
        const rect = anchor.getBoundingClientRect();
        valueEntryAnchor.value = anchor;
        if (bindValueEntry) bindValueEntry(rect);
        else manualGestures.valueEditing.open(label, value, rect, limits, apply);
    },
    closeKnobValueWithin(panel) {
        if (panel.contains(valueEntryAnchor.value ?? null)) manualGestures.cancelEntry();
    },
    registerKnobClipboard(element, knob) {
        knobClipboards.set(element, knob);
        return () => knobClipboards.delete(element);
    },
    selectedLanes, portsChanged: scheduleWireUpdate,
    elementAtPointer: () => {
        const element = trustedHover.elementAtPointer();
        return element instanceof Element ? element : null;
    },
});

function measureLaneHeader(element: Element, measurements?: Map<Element, DOMRect>): DOMRect | undefined {
    const header = element.closest(".lane-label");
    if (!header) return;
    const cached = measurements?.get(header);
    if (cached) return cached;
    const bounds = header.getBoundingClientRect();
    measurements?.set(header, bounds);
    return bounds;
}
function measureWireAnchor(element: HTMLElement, id: string, headers?: Map<Element, DOMRect>): PreviewAnchor | null {
    if (!element.isConnected || element.closest(".port-panel.positioning")) return null;
    const direction = element.closest<HTMLElement>("[data-direction]")?.dataset.direction;
    if (direction !== "input" && direction !== "output") return null;
    const circles = element.querySelectorAll<HTMLElement>(".port-circle");
    // The grouped icon has two circles. Use its outer circle on the cable side.
    const circle = circles.item(direction === "input" ? circles.length - 1 : 0) ?? element;
    const rect = circle.getBoundingClientRect();
    const header = measureLaneHeader(element, headers);
    const left = Math.max(rect.left, header?.left ?? rect.left);
    const right = Math.min(rect.right, header?.right ?? rect.right);
    if (right <= left || !rect.height) return null;
    const point: PreviewAnchor = { id, direction, x: direction === "input" ? right : left, y: rect.top + rect.height / 2,
        moduleBoundary: !!element.closest("[data-module-boundary]"),
        rulerInput: direction === "input" && !!element.closest(".timeline-snap-input") };
    const body = element.closest(".expansion-body");
    if (body) {
        const clip = body.getBoundingClientRect();
        if (point.y < clip.top || point.y > clip.bottom || point.x < clip.left || point.x > clip.right) return null;
    }
    return point;
}
function measureWireOcclusions(top: number, bottom: number, roots: readonly ParentNode[],
    headers: Map<Element, DOMRect>): WireOcclusions {
    const range = document.createRange();
    const occlusions: WireOcclusions = { labels: [], knobs: [] };
    for (const root of roots) for (const element of root.querySelectorAll(".lane-name, .lane-source-label, .direct-port-name, .port-entry .port-name, .manual-knob")) {
        if (element.closest(".port-panel.positioning")) continue;
        const bounds = element.getBoundingClientRect();
        const popup = element.closest(".expansion-body")?.getBoundingClientRect();
        const horizontalClip = popup ?? measureLaneHeader(element, headers);
        const clipTop = popup?.top ?? top;
        const clipBottom = popup?.bottom ?? bottom;
        if (!bounds.width || !bounds.height || bounds.bottom < clipTop || bounds.top > clipBottom) continue;
        // The label can fill a grid column while its text occupies only a few pixels.
        const isKnob = element.matches(".manual-knob");
        let content = bounds;
        if (!isKnob) {
            range.selectNodeContents(element);
            content = range.getBoundingClientRect();
        }
        const left = Math.max(bounds.left, content.left, horizontalClip?.left ?? bounds.left);
        const right = Math.min(bounds.right, content.right, horizontalClip?.right ?? bounds.right);
        const upper = Math.max(bounds.top, content.top, clipTop);
        const lower = Math.min(bounds.bottom, content.bottom, clipBottom);
        if (right > left && lower > upper)
            occlusions[isKnob ? "knobs" : "labels"].push(new DOMRect(left, upper, right - left, lower - upper));
    }
    return occlusions;
}
function portGroupAt(point: Point): HoveredPortGroup | null {
    const hit = document.elementFromPoint(point.x, point.y);
    const group = hit?.closest<HTMLElement>(".port-panel[data-port-group]")
        ?? hit?.closest("[data-port-control]")?.closest<HTMLElement>("[data-folded-ports]");
    const nodeKey = group?.dataset.nodeKey ?? group?.closest<HTMLElement>("[data-node-key]")?.dataset.nodeKey;
    const direction = group?.dataset.foldedPorts ?? group?.dataset.portGroup;
    const rowKey = group?.closest<HTMLElement>("[data-lane-row]")?.dataset.laneRow;
    return nodeKey && (direction === "input" || direction === "output") ? { nodeKey, direction, rowKey } : null;
}
function measurePortColorColumns(headers: Map<Element, DOMRect>): PortColorColumns[] {
    const columns: PortColorColumns[] = [];
    for (const area of document.querySelectorAll<HTMLElement>(".node-ports")) {
        const input = area.querySelector<HTMLElement>('.port-group[data-direction="input"]');
        const output = area.querySelector<HTMLElement>('.port-group[data-direction="output"]');
        if (!input || !output || !input.dataset.nodeKey) continue;
        const inputRect = input.getBoundingClientRect();
        const outputRect = output.getBoundingClientRect();
        const inputHandle = input.querySelector(".port-handle .port-circle, .port-slot")?.getBoundingClientRect() ?? inputRect;
        const outputHandle = output.querySelector(".port-handle .port-circle, .port-slot")?.getBoundingClientRect() ?? outputRect;
        if (!inputRect.height || !outputRect.height) continue;
        const header = measureLaneHeader(area, headers);
        const socketCenter = (rect: DOMRect) => Math.min(header?.right ?? Infinity,
            Math.max(header?.left ?? -Infinity, (rect.left + rect.right) / 2));
        columns.push({ nodeKey: input.dataset.nodeKey, inputX: socketCenter(inputHandle),
            outputX: socketCenter(outputHandle), top: Math.min(inputRect.top, outputRect.top),
            bottom: Math.max(inputRect.bottom, outputRect.bottom) });
    }
    return columns;
}
function updateWires() {
    const scope = visibleScope.value;
    if (!scope || !surface.value) { selectedWires.value = []; dragAnchors.value = []; connectionStartAnchor.value = null; return; }
    const drag = connectionDrag.value;
    const timeline = surface.value.querySelector(".timeline-scope");
    const viewport = (timeline ?? surface.value).getBoundingClientRect();
    const header = timeline?.querySelector<HTMLElement>(".timeline-heading");
    const heading = header?.getBoundingClientRect();
    const snapInput = header?.querySelector(".timeline-snap-input [data-port-endpoint]")
        ?.closest(".timeline-snap-input")?.getBoundingClientRect();
    const top = heading ? Math.max(viewport.top, heading.bottom) : viewport.top;
    const popovers = [...document.querySelectorAll<HTMLElement>(".port-panel")];
    const auxiliaryRoots = header ? [header, ...popovers] : popovers;
    const revealedEnds = popovers.flatMap((panel) => [...panel.querySelectorAll<HTMLElement>("[data-connection-end]")]
        .map((element) => ({ element, end: JSON.parse(element.dataset.connectionEnd!) as RevealedConnectionEnd })));
    const connections = wireConnections.value;
    let candidates: Iterable<ResolvedLaneConnections[number]> = connections;
    let anchorRoots: readonly ParentNode[] = [document];
    let occlusionRoots: readonly ParentNode[] = anchorRoots;
    const container = timeline?.querySelector<HTMLElement>(":scope > .lanes");
    if (container && !drag) {
        const lanes = readWireLaneLayout(container, connections);
        const laneBounds = container.getBoundingClientRect();
        // Use the rendered height so fractional CSS pixels cannot shift
        // the viewport range after scrolling through many rows.
        const rowHeight = lanes.rows.length && laneBounds.height
            ? laneBounds.height / lanes.rows.length : layout.value.timeline.laneHeight;
        const first = Math.floor((top - laneBounds.top) / rowHeight) - 1;
        const last = Math.floor((viewport.bottom - laneBounds.top) / rowHeight) + 1;
        const inView = lanes.index.inRows(first, last);
        if (revealedEnds.length) {
            const withRevealed = new Set(inView);
            for (const { end } of revealedEnds) {
                const connection = wireConnectionsById.value.get(end.connectionId);
                if (connection) withRevealed.add(connection);
            }
            candidates = withRevealed;
        } else candidates = inView;
        const roots = new Set(auxiliaryRoots);
        const addresses = new Set<string>();
        for (const { inputs, outputs } of candidates) {
            for (const { address } of [...inputs, ...outputs]) addresses.add(address);
        }
        for (const address of addresses) for (const row of lanes.byAddress.get(address) ?? []) roots.add(row);
        anchorRoots = [...roots];
        occlusionRoots = [...auxiliaryRoots, ...lanes.rows.slice(Math.max(0, first), Math.max(0, last + 1))];
    }
    const exact = new Map<string, PreviewAnchor[]>();
    const slots = new Map<string, PreviewAnchor[]>();
    const connectionEnds = new Map<string, PreviewAnchor[]>();
    const laneHeaders = new Map<Element, DOMRect>();
    const addAnchor = (anchors: Map<string, PreviewAnchor[]>, key: string, kind: string, element: HTMLElement) => {
        const row = element.closest<HTMLElement>("[data-lane-row]")?.dataset.laneRow;
        const anchor = measureWireAnchor(element, JSON.stringify([kind, key, row]), laneHeaders);
        if (!anchor) return;
        const existing = anchors.get(key);
        if (existing) existing.push(anchor);
        else anchors.set(key, [anchor]);
    };
    for (const { element, end } of revealedEnds) {
        const key = connectionEndKey(end.connectionId, end.port);
        addAnchor(connectionEnds, key, "connection", element);
    }
    for (const root of anchorRoots) for (const element of root.querySelectorAll<HTMLElement>("[data-port-endpoint]")) {
        const port = JSON.parse(element.dataset.portEndpoint!) as GraphPortEndpoint;
        const key = portEndpointKey(port);
        addAnchor(exact, key, "port", element);
    }
    for (const root of anchorRoots) for (const element of root.querySelectorAll<HTMLElement>("[data-port-slot]")) {
        const key = element.dataset.portSlot!;
        addAnchor(slots, key, "slot", element);
    }
    const portAnchors = (port: GraphPortEndpoint) => {
        const direct = exact.get(portEndpointKey(port));
        return direct?.length ? direct : slots.get(portSlotKey(port.address, port.direction, port.moduleBoundary)) ?? [];
    };
    const anchorFor = (port: GraphPortEndpoint, connectionId: string) =>
        connectionEnds.get(connectionEndKey(connectionId, port)) ?? portAnchors(port);
    selectedWires.value = profileGraphUi("Route connections", () => selectedLaneConnections(candidates, selectedLanes.value,
        anchorFor, revealedEnds.map(({ end }) => end), { top, bottom: viewport.bottom }));
    if (selectedWires.value.length || drag) {
        wireOcclusions.value = profileGraphUi("Measure wire occlusions", () =>
            measureWireOcclusions(top, viewport.bottom, occlusionRoots, laneHeaders));
        wireClips.value = [{ x: viewport.left, y: top, width: viewport.width, height: Math.max(0, viewport.bottom - top) },
            ...(snapInput ? [{ x: snapInput.x, y: snapInput.y, width: snapInput.width, height: snapInput.height }] : []),
            ...popovers.map((element) => {
                const rect = element.getBoundingClientRect();
                return { x: rect.left, y: rect.top, width: rect.width, height: rect.height };
            })];
    }
    const starts = drag && !drag.edit ? portAnchors(drag.port) : null;
    if (starts?.length) {
        const previous = connectionStartAnchor.value;
        connectionStartAnchor.value = starts.reduce((closest, candidate) => previous
            && Math.abs(candidate.y - previous.y) < Math.abs(closest.y - previous.y) ? candidate : closest);
    } else connectionStartAnchor.value = null;
    previewColorColumns.value = drag ? measurePortColorColumns(laneHeaders) : [];
    const end = drag?.edit;
    const anchors = end ? fixedConnectionPorts(scope, end).flatMap((port) => anchorFor(port, end.connection.id)) : [];
    dragAnchors.value = [...new Map(anchors.map((anchor) => [anchor.id, anchor])).values()];
    updateDropPreview();
}
const wireUpdate = usePresentationFrame("measure", () => {
    if (!disposed) profileGraphUi("Measure and route wires", updateWires);
});
function scheduleWireUpdate() {
    if (disposed || pendingWireUpdate) return;
    pendingWireUpdate = true;
    void nextTick(() => {
        pendingWireUpdate = false;
        if (!disposed) wireUpdate.schedule();
    });
}
watch([selectedLanes, visibleScope, lanes, () => layout.value.timeline.laneHeight],
    scheduleWireUpdate, { flush: "post" });
function startConnection(event: PointerEvent, port: GraphPortEndpoint, edit?: GraphConnectionEnd) {
    if (!surface.value || !state.value.scope?.editable || connectionDrag.value) return;
    const handle = event.currentTarget;
    if (!(handle instanceof HTMLElement)) return;
    const anchor = measureWireAnchor(handle, `port:${portEndpointKey(port)}`);
    if (!anchor) return;
    const drag = beginPortConnection(port, event.pointerId, event.button, { x: event.clientX, y: event.clientY }, edit);
    if (!drag) return;
    event.preventDefault();
    event.stopPropagation();
    // Capture source geometry before focus and before the source popover closes.
    surface.value.focus({ preventScroll: true });
    surface.value.setPointerCapture(event.pointerId);
    connectionDrag.value = drag;
    connectionStartAnchor.value = edit ? null : anchor;
    sourceGroupToLeave = { nodeKey: addressKey(port.address), direction: port.direction,
        rowKey: handle.closest<HTMLElement>("[data-lane-row]")?.dataset.laneRow };
    hoveredPortGroup.value = null;
    scheduleWireUpdate();
}
function destinationHandle(hit: Element | null): HTMLElement | null {
    return hit?.closest<HTMLElement>("[data-port-endpoint]")
        ?? hit?.closest("[data-port-drop-target]")?.querySelector<HTMLElement>("[data-port-endpoint]") ?? null;
}
function updateDropPreview() {
    const drag = connectionDrag.value;
    const handle = drag?.moved ? destinationHandle(document.elementFromPoint(drag.end.x, drag.end.y)) : null;
    const target = handle?.dataset.portEndpoint ? JSON.parse(handle.dataset.portEndpoint) as GraphPortEndpoint : null;
    dropPreviewAnchor.value = handle && drag && target && canDropPortConnection(visibleScope.value, drag, target)
        ? measureWireAnchor(handle, "drop-preview") : null;
}
function moveConnection(event: PointerEvent) {
    observePointerShift(event);
    const drag = connectionDrag.value;
    if (!drag || event.pointerId !== drag.pointerId) return;
    event.preventDefault();
    const next = movePortConnection(drag, event.pointerId, event.buttons, { x: event.clientX, y: event.clientY });
    if (!next) { cancelConnection(); return; }
    connectionDrag.value = next;
    updateDropPreview();
    const hovered = portGroupAt(next.end);
    if (sourceGroupToLeave) {
        if (hovered?.nodeKey === sourceGroupToLeave.nodeKey && hovered.rowKey === sourceGroupToLeave.rowKey
            && hovered.direction === sourceGroupToLeave.direction) {
            hoveredPortGroup.value = null;
            return;
        }
        sourceGroupToLeave = null;
    }
    if (hovered?.nodeKey !== hoveredPortGroup.value?.nodeKey || hovered?.direction !== hoveredPortGroup.value?.direction
        || hovered?.rowKey !== hoveredPortGroup.value?.rowKey) {
        hoveredPortGroup.value = hovered;
    }
}
function releaseConnection(event: PointerEvent) {
    observePointerShift(event);
    const drag = connectionDrag.value;
    if (!drag || event.pointerId !== drag.pointerId) return;
    event.preventDefault();
    if (event.button !== 0) { cancelConnection(); return; }
    const hit = document.elementFromPoint(event.clientX, event.clientY);
    const handle = destinationHandle(hit);
    const target = handle?.dataset.portEndpoint ? JSON.parse(handle.dataset.portEndpoint) as GraphPortEndpoint : null;
    if (target && !canDropPortConnection(visibleScope.value, drag, target)) {
        cancelConnection(); return;
    }
    const emptyEditorSpace = !!hit && !!surface.value?.contains(hit)
        && !hit.closest("[data-port-group], .port-panel, button, input, select, textarea, a, label, summary, [contenteditable], [role=button], [role=slider], [role=dialog], .package-dialog-backdrop");
    const connection = finishPortConnection(drag, event.pointerId,
        { x: event.clientX, y: event.clientY }, target, emptyEditorSpace);
    cancelConnection();
    if (connection?.type === "create") webviewApi.postMessage({ type: "connectGraphPorts", first: connection.first, second: connection.second });
    else if (connection?.type === "edit") webviewApi.postMessage({ type: "editGraphConnectionEnd", edit: connection.edit });
}
function cancelConnection() {
    const pointerId = connectionDrag.value?.pointerId;
    connectionDrag.value = null;
    connectionStartAnchor.value = null;
    dropPreviewAnchor.value = null;
    hoveredPortGroup.value = null;
    sourceGroupToLeave = null;
    dragAnchors.value = [];
    previewColorColumns.value = [];
    if (pointerId !== undefined && surface.value?.hasPointerCapture(pointerId)) {
        surface.value.releasePointerCapture(pointerId);
    }
}
function pointerCancelled(event: PointerEvent) {
    if (event.type === "lostpointercapture"
        && (event.target !== surface.value || surface.value?.hasPointerCapture(event.pointerId))) return;
    if (pointerLock.suppressPointerCancellation()) return;
    releaseCapturedControl(event);
    if (connectionDrag.value?.pointerId === event.pointerId) cancelConnection();
}
function pointerLeftWindow(event: PointerEvent) {
    if (knobDrag || objectDrag) { event.stopImmediatePropagation(); return; }
    if (!event.relatedTarget && (event.clientX <= 0 || event.clientY <= 0
        || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight)) pointerCancelled(event);
}
function focusSurfaceFromPointer(event: PointerEvent) {
    if (event.target instanceof Element
        && !event.target.closest("input, textarea, select, button, a, summary, [contenteditable], [tabindex]:not(.graph-editor, .timeline-scope)")) {
        surface.value?.focus({ preventScroll: true });
    }
}
function focusIdleSurface() {
    if (document.hasFocus() && document.activeElement === document.body) surface.value?.focus({ preventScroll: true });
}
function keepAltInDetachedGraph(event: KeyboardEvent) {
    // VS Code opens detached editors in a popup and forwards webview keys
    // to the workbench from a bubble listener. Keep Alt available to pointer gestures.
    if (event.key !== "Alt" || !window.top?.opener) return;
    event.preventDefault();
    event.stopImmediatePropagation();
}
function openAddNode() {
    if (!state.value.scope?.editable) return;
    cancelConnection();
    blurPointerControls();
    pickerReply.value = undefined;
    pickerCreationError.value = "";
    pickerOpen.value = true;
}
function closeNodePicker() {
    pickerOpen.value = false;
    void nextTick(() => surface.value?.focus({ preventScroll: true }));
}
function nodeConstructionCallback(definition: NodeDefinitionPreview) {
    if (definition.requiresConfiguration) {
        return selectPresentation(definition.id).createInitialConfiguration;
    }
}
function nodeCreationUnavailable(definition: NodeDefinitionPreview) {
    if (definition.requiresConfiguration && !nodeConstructionCallback(definition))
        return "Requires construction arguments";
}
function createPickedNode(definition: NodeDefinitionPreview) {
    const createConfiguration = nodeConstructionCallback(definition);
    if (definition.requiresConfiguration && !createConfiguration) return;
    pickerCreationError.value = "";
    let configurationSource = "";
    try {
        if (createConfiguration) {
            configurationSource = createConfiguration();
            if (typeof configurationSource !== "string" || !configurationSource.trim())
                throw new Error("The presentation must supply construction arguments.");
        }
    } catch (error) {
        pickerCreationError.value = error instanceof Error ? error.message : String(error);
        return;
    }
    webviewApi.postMessage({ type: "createGraphNode", definitionId: definition.id, displayName: "",
        configurationSource,
        afterInstanceId: selectedLaneIds.value.at(-1) });
    closeNodePicker();
}
function searchNodeDefinitions(requestId: string, source: string, cursor: number) {
    pickerCreationError.value = "";
    webviewApi.postMessage({ type: "searchNodeDefinitions", requestId, source, cursor });
}
function knobClipboardBlocked() {
    return knobDrag || objectDrag || connectionDrag.value || document.pointerLockElement
        || manualGestures.valueEditing.view() || isTextEditingTarget(document.activeElement);
}
function handleKnobClipboard(event: KeyboardEvent, copy: boolean): boolean {
    if (knobClipboardBlocked()) return false;
    let element = trustedHover.elementAtPointer();
    while (element instanceof Element && !knobClipboards.has(element)) element = element.parentElement;
    const target = element instanceof Element ? element : document.activeElement;
    const knob = target && knobClipboards.get(target);
    const value = knob && knob.value();
    if (!knob || value === undefined) return false;
    if (event.repeat) return true;
    if (!copy && !state.value.scope?.editable) return false;
    clearTimeout(knobClipboardStatusTimer);
    knobClipboardStatus = null;
    publishInteractionHint();
    const requestId = ++knobClipboardRequest;
    if (copy) {
        const text = Object.is(value, -0) ? "-0" : String(value);
        webviewApi.postMessage({ type: "knobClipboard", requestId, text });
    } else {
        const identity = knob.identity();
        pendingKnobPastes.set(requestId, (text) => {
            if (target?.isConnected && knobClipboards.get(target) === knob && knob.identity() === identity
                && state.value.scope?.editable && !knobClipboardBlocked()) return knob.paste(text);
        });
        webviewApi.postMessage({ type: "knobClipboard", requestId });
    }
    return true;
}
function handleGraphEditorShortcut(event: KeyboardEvent) {
    if (!state.value.scope || pickerOpen.value || editing.value !== null || creatingPackage.value || isTextEditingTarget(event.target)) return;
    if (playbackProfileMarker && performance.now() < playbackProfileMarker.until
        && event.key === "m" && !event.ctrlKey && !event.metaKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        event.stopImmediatePropagation();
        if (!event.repeat) webviewApi.postMessage({
            type: "playbackProfileClick", profileId: playbackProfileMarker.profileId,
        } satisfies GraphEditorWebviewMessage);
        return;
    }
    const action = graphEditorShortcut(event);
    if (!action || (action === "selectAll" && event.target instanceof Element
        && event.target.closest("[data-lane-item-editor]"))) return;
    const knobShortcut = action === "copyKnobValue" || action === "pasteKnobValue";
    if (knobShortcut && !handleKnobClipboard(event, action === "copyKnobValue")) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    if (knobShortcut) return;
    if (action === "selectAll") {
        selectedLanes.value = new Set(laneKeys.value);
        if (!laneSelectionAnchor || !selectedLanes.value.has(laneSelectionAnchor)) laneSelectionAnchor = laneKeys.value[0] ?? null;
        return;
    }
    if (event.repeat || !state.value.scope.editable) return;
    if (action === "addNode") openAddNode();
    else if (!pendingLaneCopies && selectedLaneIds.value.length) {
        const copies = selectedLaneIds.value.map((sourceInstanceId) => ({ sourceInstanceId, instanceId: crypto.randomUUID() }));
        save();
        const requestId = copies[0].instanceId;
        pendingLaneCopies = requestId;
        webviewApi.postMessage({ type: "duplicateSelectedLanes", copies, requestId });
    }
}
function interruptProjectHistory() {
    cancelConnection();
    for (const interrupt of presentationInterrupts) interrupt("history");
    beats.edits.abandon();
}
function handleProjectHistory(event: KeyboardEvent) {
    const direction = projectHistoryShortcut(event, isTextEditingTarget(event.target));
    if (!direction) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    save();
    webviewApi.postMessage({ type: "projectHistory", direction });
}
function keyDown(event: KeyboardEvent) {
    objectDrag?.modifiers?.(event);
    const target = event.target instanceof Element ? event.target : null;
    const editingText = isTextEditingTarget(event.target);
    const editingLaneItems = !!target?.closest("[data-lane-item-editor]");
    if (!editingText && event.code === "Space" && !event.ctrlKey && !event.metaKey && !event.altKey) {
        event.preventDefault();
        if (!event.repeat) webviewApi.postMessage({ type: "playbackToggle" });
        return;
    }
    if (event.shiftKey && ["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(event.key)
        && !event.defaultPrevented && !editingText && !event.altKey && !event.ctrlKey && !event.metaKey
        && state.value.scope?.editable && selectedLaneIds.value.length && !objectDrag) {
        event.preventDefault();
        const current = currentProjectLaneOrder();
        if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
            const next = moveLaneNesting(lanes.value, current, layout.value.laneParents, selectedLanes.value,
                event.key === "ArrowLeft" ? -1 : 1);
            if (next) {
                showLaneParents(next.parents, selectedLanes.value);
                save(next.order, next.parents);
            }
            return;
        }
        const next = moveSelectedSiblingLanes(lanes.value, current, selectedLanes.value, event.key === "ArrowUp" ? -1 : 1);
        if (next !== current) {
            save(next);
            const focused = document.activeElement;
            void nextTick(() => {
                if (focused instanceof HTMLElement && focused.isConnected && document.activeElement === document.body)
                    focused.focus({ preventScroll: true });
            });
        }
        return;
    }
    if (event.key === "Delete" && !event.defaultPrevented && !editingText
        && !editingLaneItems
        && !event.altKey && !event.ctrlKey && !event.metaKey && !event.shiftKey) {
        const instanceIds = selectedLaneIds.value;
        if (instanceIds.length) {
            event.preventDefault();
            if (!event.repeat) webviewApi.postMessage({ type: "deleteSelectedLanes", instanceIds });
        }
        return;
    }
    hoverShift.value = keyboardPortHoverShift(hoverShift.value, event.shiftKey);
    if (event.key === "Escape") { cancelConnection(); knobClicks.cancel(); pointerLock.cancel(); }
}
function keyUp(event: KeyboardEvent) {
    objectDrag?.modifiers?.(event);
    hoverShift.value = keyboardPortHoverShift(hoverShift.value, event.shiftKey);
}
function observePointerShift(event: PointerEvent) {
    hoverShift.value = pointerPortHoverShift(hoverShift.value, event);
}
function windowBlurred() {
    if (document.hasFocus()) return;
    hoverShift.value = initialPortHoverShift();
    if (!knobDrag && !objectDrag && !document.pointerLockElement && !cursorReturn.waitingForUnlock) {
        blurPointerControls();
        if (state.value.scope) {
            save();
            webviewApi.postMessage({ type: "flushProjectAutosave" } satisfies GraphEditorWebviewMessage);
        }
    }
    publishInteractionHint();
    // Keep the captured gesture. Native activation can blur this iframe again
    // immediately after a successful pointerdown; the host reports real exits.
}
function nativeWindowFocusChanged(focused: boolean) {
    if (!focused) {
        pendingKnobPastes.clear();
        hoverShift.value = initialPortHoverShift();
        cancelConnection();
        blurPointerControls();
    }
    if (focused) focusPointerHint();
    else publishInteractionHint();
}
function cancelHiddenObjectDrag() {
    if (document.hidden && objectDrag) pointerLock.cancel();
}
function createPackage() {
    webviewApi.postMessage({ type: "createGraphPackage", name: packageName.value });
    creatingPackage.value = false;
}
function receive(event: MessageEvent<GraphEditorHostMessage>) {
    const recording = graphUiRecording;
    const type = event.data?.type;
    if (!recording || (type !== "graphState" && type !== "playbackUpdate" && type !== "manualControlState"
        && type !== "observationUpdate" && type !== "observationResponse")) {
        applyMessage(event);
        return;
    }
    recording.work.measure(`Apply ${type} message`, () => applyMessage(event));
    profileVueFlush(recording);
}

function applyMessage(event: MessageEvent<GraphEditorHostMessage>) {
    if (event.data?.type === "startPlaybackProfile") {
        playbackProfileMarker = { profileId: event.data.profileId, until: performance.now() + event.data.durationMs };
        return;
    }
    if (event.data?.type === "graphPresentationAccepted") {
        const { requestId, revision } = event.data;
        const pending = pendingPresentation.get(requestId);
        if (!pending) return;
        if (revision === undefined) pendingPresentation.delete(requestId);
        else pending.revision = revision;
        updateProjectPresentation();
        return;
    }
    if (event.data?.type === "knobClipboardResult") {
        const { requestId, text, error } = event.data;
        const paste = pendingKnobPastes.get(requestId);
        pendingKnobPastes.delete(requestId);
        if (!document.hasFocus()) return;
        const problem = error ?? (text !== undefined ? paste?.(text) : undefined);
        if (problem) {
            clearTimeout(knobClipboardStatusTimer);
            knobClipboardStatus = { text: problem };
            publishInteractionHint();
            knobClipboardStatusTimer = setTimeout(() => { knobClipboardStatus = null; publishInteractionHint(); }, 3000);
        }
        return;
    }
    if (event.data?.type === "cursorReturnCompleted") {
        cursorReturn.complete(event.data.requestId);
        return;
    }
    if (event.data?.type === "playbackUpdate") {
        playback.receive(event.data);
        return;
    }
    if (event.data?.type === "graphVisibility") {
        graphVisible = event.data.visible;
        // A suspended webview may have missed replies for these reads.
        if (graphVisible) laneViewReads.clear();
        graphUiRecording?.setVisible(graphVisible);
        observations.setVisible(event.data.visible);
        return;
    }
    if (event.data?.type === "startCursorReturnTrace") {
        const { traceId } = event.data;
        clearTimeout(cursorReturnTraceTimer);
        armedCursorReturnTrace = traceId;
        cursorReturnTraceTimer = setTimeout(() => {
            armedCursorReturnTrace = undefined;
            webviewApi.postMessage({ type: "cursorReturnTrace", traceId,
                step: "Trace expired before a playhead or control-point press" } satisfies GraphEditorWebviewMessage);
        }, 30_000);
        return;
    }
    if (event.data?.type === "startGraphUiProfile") {
        clearTimeout(graphUiProfileTimer);
        graphUiRecording?.dispose();
        const recording = graphUiRecording = new GraphUiRecording();
        recording.setVisible(graphVisible);
        const { profileId, durationMs } = event.data;
        graphUiProfileTimer = setTimeout(() => {
            graphUiRecording = undefined;
            const summary = recording.finish({
                lanes: lanes.value.length, expandedModules: expandedModules.value.size,
                connections: state.value.scope?.connections.length ?? 0,
                drawnWires: selectedWires.value.length, laneHeight: layout.value.timeline.laneHeight,
            });
            webviewApi.postMessage({ type: "graphUiProfile", profileId, summary } satisfies GraphEditorWebviewMessage);
        }, durationMs);
        return;
    }
    if (event.data?.type === "startObservationProfile") {
        clearTimeout(observationProfileTimer);
        const profile = new ObservationProfile();
        observationProfile = profile;
        observations.startProfile(profile, observationLaneName);
        observationProfileTimer = setTimeout(() => {
            observationProfile = undefined;
            observationProfileTimer = undefined;
            observations.stopProfile();
            webviewApi.postMessage({ type: "observationProfile", summary: profile.summary(observations.values) } satisfies GraphEditorWebviewMessage);
        }, event.data.durationMs);
        return;
    }
    if (event.data?.type === "startClientInteractionTrace") {
        clearTimeout(clientTraceTimer);
        clientTrace?.stop();
        const { traceId, durationMs } = event.data;
        const recording = new ClientInteractionRecorder(observations);
        clientTrace = recording;
        clientTraceTimer = setTimeout(() => {
            clientTrace = undefined;
            const message: GraphEditorWebviewMessage = { type: "clientInteractionTrace", traceId, recording: recording.stop() };
            webviewApi.postMessage(message);
        }, durationMs);
        return;
    }
    if (event.data?.type === "observationUpdate") {
        const { update, sequence, previousSequence } = event.data;
        const delivery = observationDelivery.receive(update, sequence, previousSequence);
        if (delivery === "resync") webviewApi.postMessage({ type: "observationResync" } satisfies GraphEditorWebviewMessage);
        const changed = delivery === "applied" && observations.receiveUpdate(update);
        observationProfile?.record(event.data, changed);
        return;
    }
    if (event.data?.type === "observationResponse") {
        observations.receive(event.data);
        return;
    }
    if (event.data?.type === "manualControlState") {
        manualGestures.receive(event.data.interactions, event.data.live);
        if (knobDrag?.active && !knobDrag.active()) pointerLock.cancel();
        return;
    }
    if (event.data?.type === "graphAction") {
        if (event.data.action === "addNode") { openAddNode(); return; }
        cancelConnection();
        blurPointerControls();
        const action = event.data;
        if (action.action === "editNode" && action.instanceId
            && state.value.scope?.nodes.some((node) => node.instance.instanceId === action.instanceId)) {
            editing.value = action.instanceId;
        }
        if (action.action === "createPackage") { packageName.value = ""; creatingPackage.value = true; }
        return;
    }
    if (event.data?.type === "nodeDefinitionSearchResult") {
        pickerReply.value = event.data;
        return;
    }
    if (event.data?.type === "graphLanesInserted") {
        if (event.data.duplicateRequestId !== undefined) {
            if (pendingLaneCopies !== event.data.duplicateRequestId) return;
            pendingLaneCopies = undefined;
        }
        if (event.data.lanes.length) selectNewLanes(event.data.lanes);
        return;
    }
    if (event.data?.type === "projectPresentations") {
        replaceProjectPresentations(event.data.plugins);
        return;
    }
    if (event.data?.type === "laneViewState") {
        const { version, requestId, state } = event.data;
        if (laneViewReads.get(version) === requestId && state !== null) {
            laneViewStates.set(version, state);
            laneViewReads.delete(version);
        }
        return;
    }
    if (event.data?.type === "interruptProjectHistory") {
        const { requestId } = event.data;
        if (requestId <= finishedProjectHistoryRequest || requestId < (projectHistoryRequest.value ?? 0)) return;
        if (projectHistoryRequest.value !== requestId) {
            projectHistoryRequest.value = requestId;
            interruptProjectHistory();
        }
        webviewApi.postMessage({ type: "projectHistoryInterrupted", requestId } satisfies GraphEditorWebviewMessage);
        return;
    }
    if (event.data?.type === "projectHistoryFinished") {
        finishedProjectHistoryRequest = Math.max(finishedProjectHistoryRequest, event.data.requestId);
        if (projectHistoryRequest.value !== undefined && projectHistoryRequest.value <= event.data.requestId)
            projectHistoryRequest.value = undefined;
        return;
    }
    if (event.data?.type !== "graphState") return;
    if (state.value.connectionNumber !== event.data.state.connectionNumber) {
        laneViewReads.clear();
        laneViewStates.clear();
    }
    state.value = event.data.state;
    const versions = new Set(laneViewVersions.value.values());
    if (state.value.sessionStatus !== "running") versions.clear();
    for (const version of laneViewReads.keys()) if (!versions.has(version)) laneViewReads.delete(version);
    for (const version of laneViewStates.keys()) if (!versions.has(version)) laneViewStates.delete(version);
    if (editing.value !== null && !editedNode.value) editing.value = null;
    if (!state.value.scope?.editable && pickerOpen.value) closeNodePicker();
    manualGestures.configuredGeneration(state.value.scope?.generation ?? null);
    const scope = state.value.scope;
    if (state.value.sessionStatus === "running") {
        const configuration = observationConfiguration(scope);
        if (configuration !== configuredObservations) {
            configuredObservations = configuration;
            observations.refresh();
        }
    } else {
        configuredObservations = undefined;
        observations.disconnect();
        for (const interrupt of presentationInterrupts) interrupt("disconnected");
    }
    const entry = manualGestures.numericEntry();
    if (entry && !canEditManualControl(entry.control, resolveManualControl(visibleScope.value, entry.control)))
        manualGestures.cancelEntry();
    if (state.value.sessionStatus !== "running") {
        pendingPresentation.clear();
        changedPresentationStates.clear();
        pendingKnobPastes.clear();
        manualGestures.disconnected();
        pendingLaneCopies = undefined;
        insertedSelection = [];
    }
    const drag = connectionDrag.value;
    if (drag && (state.value.sessionStatus !== "running"
        || !canContinuePortConnection(visibleScope.value, drag.port, drag.edit))) cancelConnection();
    if (selectedLanes.value.size) {
        const addresses = new Set(laneKeys.value);
        const remaining = new Set([...selectedLanes.value].filter((key) => addresses.has(key)));
        if (remaining.size !== selectedLanes.value.size) selectedLanes.value = remaining;
    }
    if (editing.value && !state.value.scope?.nodes.some((entry) => entry.instance.instanceId === editing.value)) {
        editing.value = null;
    }
    updateProjectPresentation();
    selectInsertedLanes();
}
onMounted(() => {
    // Give VS Code a persisted panel state so its serializer can restore
    // this editor. Shared lane state comes from the server.
    webviewApi.setState({ ...webviewApi.getState(), surface: "graphEditor" });
    window.addEventListener("message", receive);
    window.addEventListener("pointermove", moveConnection, true);
    window.addEventListener("pointermove", moveCapturedControl, true);
    window.addEventListener("pointerup", releaseCapturedControl, true);
    window.addEventListener("mousemove", lockedControlMove, true);
    window.addEventListener("mouseup", lockedControlRelease, true);
    document.addEventListener("pointerlockchange", controlLockChanged);
    document.addEventListener("pointerlockerror", controlLockFailed);
    document.addEventListener("visibilitychange", cancelHiddenObjectDrag);
    window.addEventListener("pointerdown", clickOutsideManual, true);
    window.addEventListener("pointerdown", selectLaneAtPointer, true);
    window.addEventListener("click", clickModuleFooterAcrossSeam, true);
    window.addEventListener("contextmenu", guardDragContextMenu, true);
    window.addEventListener("wheel", wheelControl, { capture: true, passive: false });
    window.addEventListener("focus", focusPointerHint);
    window.addEventListener("focus", focusIdleSurface);
    window.addEventListener("pointerdown", observePointerShift, true);
    window.addEventListener("pointerup", releaseConnection, true);
    window.addEventListener("pointercancel", pointerCancelled, true);
    window.addEventListener("pointerout", pointerLeftWindow, true);
    window.addEventListener("keydown", keepAltInDetachedGraph, true);
    window.addEventListener("keyup", keepAltInDetachedGraph, true);
    // VS Code cancels and forwards undo/redo from an earlier window bubble listener.
    window.addEventListener("keydown", handleProjectHistory, true);
    window.addEventListener("keydown", handleGraphEditorShortcut, true);
    window.addEventListener("keydown", keyDown);
    window.addEventListener("keyup", keyUp);
    stopListeningForFocus = listenPortConnectionFocus(window, windowBlurred, nativeWindowFocusChanged);
    window.addEventListener("scroll", scheduleWireUpdate, true);
    wireResizeObserver = new ResizeObserver(scheduleWireUpdate);
    if (surface.value) wireResizeObserver.observe(surface.value);
    focusIdleSurface();
    webviewApi.postMessage({ type: "graphReady" });
});
onBeforeUnmount(() => {
    clearTimeout(knobClipboardStatusTimer);
    pendingKnobPastes.clear();
    clearTimeout(cursorReturnTraceTimer);
    clearTimeout(clientTraceTimer);
    clientTrace?.stop();
    clearTimeout(observationProfileTimer);
    observationProfile = undefined;
    observations.stopProfile();
    clearTimeout(graphUiProfileTimer);
    graphUiRecording?.dispose();
    graphUiRecording = undefined;
    observations.dispose();
    disposed = true;
    blurPointerControls();
    for (const type of pointerLockExitTypes) window.removeEventListener(type, guardPointerLockExit, true);
    trustedHover.dispose();
    webviewApi.postMessage({ type: "graphInteractionHint", hint: null });
    wireUpdate.cancel();
    wireResizeObserver?.disconnect();
    window.removeEventListener("scroll", scheduleWireUpdate, true);
    window.removeEventListener("message", receive);
    cancelConnection();
    window.removeEventListener("pointermove", moveConnection, true);
    window.removeEventListener("pointermove", moveCapturedControl, true);
    window.removeEventListener("pointerup", releaseCapturedControl, true);
    window.removeEventListener("mousemove", lockedControlMove, true);
    window.removeEventListener("mouseup", lockedControlRelease, true);
    document.removeEventListener("pointerlockchange", controlLockChanged);
    document.removeEventListener("pointerlockerror", controlLockFailed);
    document.removeEventListener("visibilitychange", cancelHiddenObjectDrag);
    window.removeEventListener("pointerdown", clickOutsideManual, true);
    window.removeEventListener("pointerdown", selectLaneAtPointer, true);
    window.removeEventListener("click", clickModuleFooterAcrossSeam, true);
    window.removeEventListener("contextmenu", guardDragContextMenu, true);
    window.removeEventListener("wheel", wheelControl, true);
    window.removeEventListener("focus", focusPointerHint);
    window.removeEventListener("focus", focusIdleSurface);
    window.removeEventListener("pointerdown", observePointerShift, true);
    window.removeEventListener("pointerup", releaseConnection, true);
    window.removeEventListener("pointercancel", pointerCancelled, true);
    window.removeEventListener("pointerout", pointerLeftWindow, true);
    window.removeEventListener("keydown", keepAltInDetachedGraph, true);
    window.removeEventListener("keyup", keepAltInDetachedGraph, true);
    window.removeEventListener("keydown", handleProjectHistory, true);
    window.removeEventListener("keydown", handleGraphEditorShortcut, true);
    window.removeEventListener("keydown", keyDown);
    window.removeEventListener("keyup", keyUp);
    stopListeningForFocus?.();
});
</script>

<template>
    <main ref="surface" class="graph-editor" tabindex="-1" @pointerdown.capture="focusSurfaceFromPointer"
        @lostpointercapture="pointerCancelled">
        <div v-if="state.sessionStatus === 'stopped'" class="status" role="status">
            The server stopped. Run Intravenous: Retry Server or reload the window.
        </div>
        <div v-else-if="state.sessionStatus !== 'running'" class="status" role="status">
            {{ state.sessionStatus === 'starting' ? 'Starting Intravenous…' : 'Restarting Intravenous…' }}
        </div>
        <div v-if="state.error" class="error" role="alert">{{ state.error }}</div>
        <PresentationHost v-if="state.scope" :node="root" view-choice="editable" />
        <ManualValuePopover :entry="manualValueEntry"
            :target="valueEntryAnchor?.closest<HTMLElement>(':popover-open, [role=dialog]') ?? undefined"
            :lane-key="valueEntryAnchor?.closest<HTMLElement>('[data-lane-key]')?.dataset.laneKey"
            :return-focus="valueEntryAnchor" :accept="() => manualGestures.acceptEntry()"
            :cancel="() => manualGestures.cancelEntry()" :set-draft="(draft) => manualGestures.setEntryDraft(draft)" />
        <NodeDefinitionPicker v-if="pickerOpen" :catalog="state.definitions" :usage="state.definitionUsage"
            :response="pickerReply" :unavailable-reason="nodeCreationUnavailable" :creation-error="pickerCreationError"
            @search="searchNodeDefinitions" @choose="createPickedNode" @close="closeNodePicker" />
        <NodeEditor v-if="editedNode" :key="editedNode.instance.instanceId" :node="editedNode" @close="editing = null" />
        <Teleport to="body">
            <div v-if="creatingPackage" class="package-dialog-backdrop" @click.self="creatingPackage = false" @keydown.esc.stop="creatingPackage = false">
                <form class="package-dialog" role="dialog" aria-modal="true" aria-label="New package" @submit.prevent="createPackage">
                    <label>Package name<input v-model="packageName" tabindex="-1" autofocus required /></label>
                    <button type="button" tabindex="-1" @click="creatingPackage = false">Cancel</button>
                    <button type="submit" tabindex="-1" :disabled="!packageName.trim()">Create</button>
                </form>
            </div>
        </Teleport>
        <svg v-if="selectedWires.length || connectionDrag?.moved" class="connection-wire" aria-hidden="true">
            <defs>
                <clipPath id="selected-wire-clip" clipPathUnits="userSpaceOnUse">
                    <rect v-for="(clip, index) in wireClips" :key="index" :x="clip.x" :y="clip.y" :width="clip.width" :height="clip.height" />
                </clipPath>
                <mask id="wire-knob-mask" maskUnits="userSpaceOnUse" x="0" y="0" width="100%" height="100%">
                    <rect width="100%" height="100%" fill="white" />
                    <rect v-for="(knob, index) in wireOcclusions.knobs" :key="index"
                        :x="knob.x" :y="knob.y - 1" :width="knob.width" :height="knob.height + 2" fill="black" />
                </mask>
                <filter id="wire-label-blur" filterUnits="userSpaceOnUse" x="0" y="0" width="100%" height="100%">
                    <feGaussianBlur stdDeviation="2" />
                </filter>
                <mask id="wire-label-fade" maskUnits="userSpaceOnUse" x="0" y="0" width="100%" height="100%">
                    <rect width="100%" height="100%" fill="white" />
                    <g v-if="wireOcclusions.labels.length" filter="url(#wire-label-blur)" fill="black"
                        :opacity="1 - wireOpacity.faint / wireOpacity.emphasized">
                        <rect v-for="(label, index) in wireOcclusions.labels" :key="index"
                            :x="label.x - 4" :y="label.y - 4" :width="label.width + 8" :height="label.height + 8" />
                    </g>
                </mask>
                <linearGradient v-for="{ wire, index } in [...wireLayers.faint, ...wireLayers.emphasized]" :id="`selected-wire-${index}`" :key="wire.key"
                    gradientUnits="userSpaceOnUse" :x1="wire.source.x" :y1="wire.source.y" :x2="wire.target.x" :y2="wire.target.y">
                    <stop offset="0" :style="{ stopColor: wireColor('output') }" />
                    <stop offset="1" :style="{ stopColor: wireColor('input') }" />
                </linearGradient>
                <linearGradient v-for="(wire, index) in previewWires" :id="`preview-wire-${index}`" :key="wire.anchor.id"
                    gradientUnits="userSpaceOnUse" :x1="wire.anchor.x" :y1="wire.anchor.y" :x2="wire.cursor.x" :y2="wire.cursor.y">
                    <stop offset="0" :style="{ stopColor: wireColor(wire.anchor.direction) }" />
                    <stop offset="1" :style="{ stopColor: cursorWireColor }" />
                </linearGradient>
                <linearGradient v-for="(wire, index) in dropPreviewWires" :id="`drop-preview-wire-${index}`" :key="`drop-${wire.anchor.id}`"
                    gradientUnits="userSpaceOnUse" :x1="wire.anchor.x" :y1="wire.anchor.y" :x2="wire.target.x" :y2="wire.target.y">
                    <stop offset="0" :style="{ stopColor: wireColor(wire.anchor.direction) }" />
                    <stop offset="1" :style="{ stopColor: wireColor(wire.target.direction) }" />
                </linearGradient>
            </defs>
            <g mask="url(#wire-knob-mask)">
                <g clip-path="url(#selected-wire-clip)">
                    <g mask="url(#wire-label-fade)">
                        <path v-for="wire in sourceWires" :key="wire.key" class="selected-wire"
                            :d="selectedLaneWirePath(wire)" stroke="var(--vscode-descriptionForeground)"
                            :stroke-opacity="wire.emphasized ? .82 : .48" />
                    </g>
                    <g v-for="(wires, layer) in wireLayers" :key="layer"
                        :mask="layer === 'emphasized' ? 'url(#wire-label-fade)' : undefined">
                        <path v-for="{ wire, index } in wires" :key="wire.key" class="selected-wire"
                            :class="{ pending: !wire.applied }"
                            :d="selectedLaneWirePath(wire)" :stroke="`url(#selected-wire-${index})`"
                            :stroke-opacity="wireOpacity[layer]" />
                    </g>
                </g>
                <g mask="url(#wire-label-fade)">
                    <path v-for="(wire, index) in dropPreviewWires" :key="`drop-${wire.anchor.id}`" class="drop-preview-wire"
                        :d="wire.path" :stroke="`url(#drop-preview-wire-${index})`" />
                    <path v-for="(wire, index) in previewWires" :key="wire.anchor.id" class="preview-wire"
                        :d="wire.path" :stroke="`url(#preview-wire-${index})`" />
                </g>
            </g>
        </svg>
    </main>
</template>

<style scoped>
.graph-editor { height: 100vh; display: flex; flex-direction: column; background: var(--vscode-editor-background); }
.graph-editor :deep(> .presentation-host) { flex: 1; min-height: 0; }
.status, .error { padding: 8px; }
.error { color: var(--vscode-errorForeground); }
.connection-wire { position: fixed; inset: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 30; }
.preview-wire { fill: none; stroke-width: 2; }
.drop-preview-wire { fill: none; stroke-width: 2; stroke-opacity: .4; }
.selected-wire { fill: none; stroke-width: 1.5; }
.selected-wire.pending { stroke-dasharray: 5 4; }
.package-dialog-backdrop { position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; background: #0005; }
.package-dialog { padding: 16px; display: flex; flex-wrap: wrap; gap: 8px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); border: 1px solid var(--vscode-panel-border); }
.package-dialog label { flex-basis: 100%; display: grid; gap: 5px; }
.package-dialog input { padding: 4px; color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border, transparent); }
</style>

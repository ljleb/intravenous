<script setup lang="ts">
import { PresentationObservations, provideObservations } from "../presentation/observationContext";
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch } from "vue";
import type { GraphEditorHostMessage, GraphEditorState } from "../../src/graphEditorProtocol";
import { addressKey, readLayout, type Point } from "../../src/graphScope";
import { webviewApi } from "../host";
import PresentationHost from "../presentation/PresentationHost.vue";
import type { SemanticNode } from "../presentation/semanticNode";
import { provideGraphEditor } from "./context";
import NodeEditor from "./NodeEditor.vue";
import { fixedConnectionPorts, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { beginPortConnection, canDropPortConnection, finishPortConnection, movePortConnection, type PortConnectionDrag } from "../../src/portConnectionGesture";
import { initialPortHoverShift, keyboardPortHoverShift, pointerPortHoverShift } from "../../src/portHoverShift";
import { connectionCursorOutputFraction, type PortColorColumns } from "../../src/connectionPreview";
import { ManualControlGestures } from "../../src/manualControlGesture";
import { knobScrubSpeed } from "../../src/knobPresentation";
import { provideManualControls } from "../presentation/manualControlContext";
import ManualValuePopover from "../presentation/ManualValuePopover.vue";
import { manualControlKey, resolveManualControl } from "../../src/manualControls";
import { KnobPointerLock } from "../../src/knobPointerLock";
import { KnobClicks } from "../../src/knobClicks";
import { PointerLockExitGuard } from "../../src/pointerLockExitGuard";
import { PointerLockExitRedirect } from "../../src/pointerLockExitRedirect";
import { TrustedHover } from "../../src/trustedHover";
import { providePlayback } from "../timeline/playbackContext";
import { useAutomationEditing } from "../presentation/automationEditingContext";
import { automationPointStatus, useElementStatusHint } from "../statusHints";
import { projectHistoryShortcut } from "../../src/projectHistoryShortcut";
import { listenPortConnectionFocus } from "../../src/portConnectionFocus";
import { connectionEndKey, connectionWirePath, portEndpointKey, portSlotKey, selectedLaneConnections, selectedLaneWirePath,
    type PortWireAnchor, type RevealedConnectionEnd, type SelectedLaneWire } from "../../src/selectedLaneConnections";

type HoveredPortGroup = { nodeKey: string; direction: "input" | "output" };
type PreviewAnchor = PortWireAnchor & { direction: "input" | "output" };

const state = shallowRef<GraphEditorState>({ sessionStatus: "starting", scope: null, layout: readLayout(null), definitions: [], error: "" });
const playback = providePlayback();
const automation = useAutomationEditing();
const elementStatus = useElementStatusHint();
onBeforeUnmount(() => playback.dispose());
const observations = new PresentationObservations((request) => webviewApi.postMessage(request), {
    initial: webviewApi.getState()?.meterModes,
    save: (meterModes) => webviewApi.setState({ ...webviewApi.getState(), meterModes }) });
provideObservations(observations);
const layout = ref(readLayout(null));
const manualVersion = ref(0);
let lastInteractionHint: string | null | undefined;
let manualLockError = "";
const knobClicks = new KnobClicks();
const manualGestures = new ManualControlGestures(
    (action) => webviewApi.postMessage({ type: "manualControlAction", action }),
    () => crypto.randomUUID(), () => { ++manualVersion.value; publishInteractionHint(); }, {
        initial: webviewApi.getState()?.outputInversions,
        save: (outputInversions) => webviewApi.setState({ ...webviewApi.getState(), outputInversions }),
    });
const manualPointer = new KnobPointerLock({
    request: () => surface.value!.requestPointerLock(),
    exit: () => document.exitPointerLock(),
    locked: () => !!surface.value && document.pointerLockElement === surface.value,
}, (id, delta) => manualGestures.moveBy(id, delta), (id, point, lockOrigin) => {
    if (lockOrigin) beginPointerLockExit(lockOrigin);
    if (!point) knobClicks.cancel();
    const control = manualGestures.pointerControl();
    const knob = point && document.elementFromPoint(point.x, point.y)?.closest<HTMLElement>(".manual-input");
    manualGestures.release(id);
    if (!control || !knob || knob.dataset.manualControl !== manualControlKey(control)) manualGestures.hover(null);
    if (surface.value?.hasPointerCapture(id)) surface.value.releasePointerCapture(id);
}, (message) => {
    manualLockError = message; publishInteractionHint();
});
provideManualControls({ gestures: manualGestures, version: manualVersion, startKnob: (event, control, descriptor, limits) => {
    if (manualGestures.pointerControl()) return;
    surface.value?.focus({ preventScroll: true });
    manualGestures.hover(control, descriptor, limits);
    const origin = { x: event.clientX, y: event.clientY };
    if (event.pointerType === "mouse" && knobClicks.press(manualControlKey(control), origin, performance.now(), event.detail)) {
        manualGestures.resetHovered();
        return;
    }
    const scale = knobScrubSpeed(limits);
    if (!manualGestures.scrub(control, descriptor, event.pointerId, -event.clientY, scale, limits, event.ctrlKey)) return;
    surface.value?.setPointerCapture(event.pointerId);
    manualLockError = "";
    manualPointer.start(event.pointerId, origin, event.pointerType === "mouse");
} });
function publishInteractionHint() {
    const manual = document.hasFocus() ? manualGestures.hint()?.text : null;
    const adjustingKnob = manualGestures.pointerControl();
    const point = document.hasFocus() ? automation.edits.activePoint() : null;
    const drag = playback.dragPosition.value;
    const hint = (manual && (adjustingKnob || manualGestures.numericEntry())
        ? manual + (adjustingKnob && manualLockError ? ` · Pointer lock unavailable. ${manualLockError}` : "") : null)
        ?? (point ? automationPointStatus(point, state.value.scope?.sampleRate) : null)
        ?? (drag === null ? null : `Start ${drag} smp`)
        ?? elementStatus.text.value ?? manual ?? null;
    if (hint === lastInteractionHint) return;
    lastInteractionHint = hint;
    webviewApi.postMessage({ type: "graphInteractionHint", hint });
}
watch([elementStatus.text, playback.dragPosition, automation.version], publishInteractionHint);
function focusManualHint() {
    if (document.hasFocus()) manualPointer.retryAfterFocus();
    lastInteractionHint = undefined; publishInteractionHint();
}
function moveManual(event: PointerEvent) {
    if (!manualGestures.pointerControl()) return;
    if (manualGestures.pointerId() !== event.pointerId) return;
    event.stopImmediatePropagation();
    if (document.pointerLockElement === surface.value) return;
    if (!(event.buttons & 1)) { releaseManual(event); return; }
    knobClicks.move(event.movementX, event.movementY);
    manualGestures.move(event.pointerId, -event.clientY);
}
function releaseManual(event: PointerEvent) {
    if (manualGestures.pointerId() === event.pointerId) knobClicks.release(performance.now());
    manualPointer.release(event.pointerId, { x: event.clientX, y: event.clientY });
}
function lockedManualMove(event: MouseEvent) {
    if (document.pointerLockElement !== surface.value) return;
    event.stopImmediatePropagation();
    // A synthesized move with an empty buttons mask is not a mouse release.
    // Mouseup, pointerup, native focus loss and pointerlockchange own termination.
    knobClicks.move(event.movementX, event.movementY);
    manualPointer.move(-event.movementY);
}
function lockedManualRelease(event: MouseEvent) {
    const id = manualGestures.pointerId();
    if (event.button === 0 && id !== null) {
        knobClicks.release(performance.now());
        manualPointer.release(id, { x: event.clientX, y: event.clientY });
    }
}
function manualLockChanged() {
    manualPointer.changed();
    if (document.pointerLockElement === surface.value) {
        // Capture is only a fallback while the lock is pending. A successful
        // lock owns relative mouse input and must not retain a capture target.
        const id = manualGestures.pointerId();
        if (id !== null && surface.value?.hasPointerCapture(id)) surface.value.releasePointerCapture(id);
        manualLockError = "";
        publishInteractionHint();
    }
}
function manualLockFailed() { manualPointer.error(); }
function wheelManual(event: WheelEvent) {
    if (!manualGestures.pointerControl()) return;
    event.preventDefault(); event.stopImmediatePropagation();
    manualGestures.changeSensitivity(event.deltaY || event.deltaX);
}
function clickOutsideManual(event: PointerEvent) {
    const target = event.target instanceof Element ? event.target : null;
    if (target?.closest(".manual-value-popover")) return;
    manualGestures.cancelEntry();
    if (!target?.closest(".manual-input") || event.button !== 0) knobClicks.cancel();
    if (!target?.closest(".manual-input")) { manualPointer.cancel(); manualGestures.finish(); }
}
function blurManual() {
    knobClicks.cancel(); manualPointer.cancel(); manualGestures.finish(); manualGestures.cancelEntry(); manualGestures.hover(null);
    // Positions after focus returns come from real movement. The cleared knob
    // hover must be entered again by the next move, even within the same knob.
    pointerLockExit.end();
    trustedHover.reset();
}

// Until the first real movement after a knob's pointer lock ends, the host
// reports a displaced position while the cursor is visibly at the lock origin.
// Moves and boundary events at that position are dropped, and presses, releases
// and clicks are delivered at the origin. Hover never follows the displaced
// position because it comes only from TrustedHover, registered after this guard.
const pointerLockExit = new PointerLockExitGuard();
const pointerLockExitTypes = [
    "pointermove", "mousemove", "pointerover", "pointerout", "pointerenter", "pointerleave",
    "mouseover", "mouseout", "mouseenter", "mouseleave",
    "pointerdown", "pointerup", "mousedown", "mouseup", "click", "dblclick", "auxclick", "contextmenu",
];
function beginPointerLockExit(origin: Point) { pointerLockExit.begin(origin); }
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
const trustedHover = new TrustedHover(undefined, elementStatus.hover);

let initializedLayout = false;
const surface = ref<HTMLElement | null>(null);
const connectionDrag = shallowRef<PortConnectionDrag | null>(null);
const hoverShift = shallowRef(initialPortHoverShift());
const connectionStartAnchor = shallowRef<PortWireAnchor | null>(null);
const dropPreviewAnchor = shallowRef<PreviewAnchor | null>(null);
const hoveredPortGroup = shallowRef<HoveredPortGroup | null>(null);
const previewColorColumns = shallowRef<PortColorColumns[]>([]);
let sourceGroupToLeave: HoveredPortGroup | null = null;
const selectedLane = ref<string | null>(null);
const selectedWires = shallowRef<SelectedLaneWire[]>([]);
const dragAnchors = shallowRef<PreviewAnchor[]>([]);
const wireClips = shallowRef<Array<{ x: number; y: number; width: number; height: number }>>([]);
const wireOcclusions = shallowRef<DOMRect[]>([]);
let wireFrame = 0;
let disposed = false;
let wireResizeObserver: ResizeObserver | null = null;
let stopListeningForFocus: (() => void) | undefined;
const editing = ref<string | null>(null); // Empty string creates an instance.
const creatingPackage = ref(false);
const packageName = ref("");
const editedNode = computed(() => state.value.scope?.nodes.find((entry) => entry.instance.instanceId === editing.value));
const root = computed<SemanticNode>(() => ({
    id: "", instanceId: "", definitionId: "", kind: "Project graph", sourceIdentity: "",
    memberCount: 0, ports: [], members: [], sourceSpans: [], scope: state.value.scope ?? undefined,
}));
const cursorOutputFraction = computed(() => connectionDrag.value
    ? connectionCursorOutputFraction(connectionDrag.value.end, previewColorColumns.value) : 0.5);
function wireColor(direction: "input" | "output", kind?: "sample" | "event" | "mixed"): string {
    return `var(--iv-${kind === "event" ? "event" : "sample"}-${direction})`;
}
const cursorWireColor = computed(() => {
    const kind = connectionDrag.value?.port.kind;
    return `color-mix(in srgb, ${wireColor("input", kind)} ${(1 - cursorOutputFraction.value) * 100}%, ${wireColor("output", kind)} ${cursorOutputFraction.value * 100}%)`;
});
const previewWires = computed(() => {
    const drag = connectionDrag.value;
    if (!drag?.moved) return [];
    const anchors: PreviewAnchor[] = drag.edit ? dragAnchors.value : connectionStartAnchor.value
        ? [{ ...connectionStartAnchor.value, direction: drag.port.direction }] : [];
    return anchors.map((anchor) => ({ anchor, cursor: drag.end,
        path: connectionWirePath(anchor, drag.end),
    }));
});
const dropPreviewWires = computed(() => {
    const target = dropPreviewAnchor.value;
    return target ? previewWires.value.map((wire) => ({ ...wire, target,
        path: connectionWirePath(wire.anchor, target) })) : [];
});
function save() { webviewApi.postMessage({ type: "saveGraphLayout", layout: readLayout(layout.value) }); }
provideGraphEditor({ layout, save,
    copyNodeDebugDetails: (address) => webviewApi.postMessage({ type: "copyNodeDebugDetails", nodeKey: addressKey(address) }),
    scope: computed(() => state.value.scope), shiftHeld: computed(() => hoverShift.value.held),
    connectionDrag, hoveredPortGroup, startConnection, cancelConnection,
    selectedLane, selectLane: (address) => { selectedLane.value = addressKey(address); }, portsChanged: scheduleWireUpdate,
});

function measureWireAnchor(element: HTMLElement, id: string, direction: "input" | "output"): PortWireAnchor | null {
    if (!element.isConnected || element.closest(".port-panel.positioning")) return null;
    const circles = element.querySelectorAll<HTMLElement>(".port-circle");
    // The grouped icon has two circles. Use its outer circle on the cable side.
    const circle = circles.item(direction === "input" ? circles.length - 1 : 0) ?? element;
    const rect = circle.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    const point = { id, x: direction === "input" ? rect.right : rect.left, y: rect.top + rect.height / 2 };
    const body = element.closest(".expansion-body");
    if (body) {
        const clip = body.getBoundingClientRect();
        if (point.y < clip.top || point.y > clip.bottom || point.x < clip.left || point.x > clip.right) return null;
    }
    return point;
}
function measureWireOcclusions(top: number, bottom: number): DOMRect[] {
    const range = document.createRange();
    const occlusions: DOMRect[] = [];
    for (const element of document.querySelectorAll(".lane-name, .direct-port-name, .port-entry .port-name, .manual-knob")) {
        const bounds = element.getBoundingClientRect();
        const popup = element.closest(".expansion-body")?.getBoundingClientRect();
        const clipTop = popup?.top ?? top;
        const clipBottom = popup?.bottom ?? bottom;
        if (!bounds.width || !bounds.height || bounds.bottom < clipTop || bounds.top > clipBottom) continue;
        // The label can fill a grid column while its text occupies only a few pixels.
        range.selectNodeContents(element);
        const content = element.matches(".manual-knob") ? bounds : range.getBoundingClientRect();
        const left = Math.max(bounds.left, content.left, popup?.left ?? bounds.left);
        const right = Math.min(bounds.right, content.right, popup?.right ?? bounds.right);
        const upper = Math.max(bounds.top, content.top, clipTop);
        const lower = Math.min(bounds.bottom, content.bottom, clipBottom);
        if (right > left && lower > upper) occlusions.push(new DOMRect(left, upper, right - left, lower - upper));
    }
    return occlusions;
}
function portGroupAt(point: Point): HoveredPortGroup | null {
    const hit = document.elementFromPoint(point.x, point.y);
    const group = hit?.closest<HTMLElement>("[data-folded-ports], .port-panel[data-port-group]");
    const nodeKey = group?.dataset.nodeKey ?? group?.closest<HTMLElement>("[data-node-key]")?.dataset.nodeKey;
    const direction = group?.dataset.foldedPorts ?? group?.dataset.portGroup;
    return nodeKey && (direction === "input" || direction === "output") ? { nodeKey, direction } : null;
}
function measurePortColorColumns(): PortColorColumns[] {
    const columns: PortColorColumns[] = [];
    for (const area of document.querySelectorAll<HTMLElement>(".node-ports")) {
        const input = area.querySelector<HTMLElement>('.port-group[data-port-group="input"]');
        const output = area.querySelector<HTMLElement>('.port-group[data-port-group="output"]');
        if (!input || !output || !input.dataset.nodeKey) continue;
        const inputRect = input.getBoundingClientRect();
        const outputRect = output.getBoundingClientRect();
        const inputHandle = input.querySelector(".port-handle .port-circle, .port-slot")?.getBoundingClientRect() ?? inputRect;
        const outputHandle = output.querySelector(".port-handle .port-circle, .port-slot")?.getBoundingClientRect() ?? outputRect;
        if (!inputRect.height || !outputRect.height) continue;
        columns.push({ nodeKey: input.dataset.nodeKey, inputX: (inputHandle.left + inputHandle.right) / 2,
            outputX: (outputHandle.left + outputHandle.right) / 2, top: Math.min(inputRect.top, outputRect.top),
            bottom: Math.max(inputRect.bottom, outputRect.bottom) });
    }
    return columns;
}
function updateWires() {
    const scope = state.value.scope;
    if (!scope || !surface.value) { selectedWires.value = []; dragAnchors.value = []; connectionStartAnchor.value = null; return; }
    const exact = new Map<string, PortWireAnchor>();
    const slots = new Map<string, PortWireAnchor>();
    const connectionEnds = new Map<string, PortWireAnchor>();
    const revealedEnds: RevealedConnectionEnd[] = [];
    for (const element of document.querySelectorAll<HTMLElement>("[data-connection-end]")) {
        const end = JSON.parse(element.dataset.connectionEnd!) as RevealedConnectionEnd;
        revealedEnds.push(end);
        const key = connectionEndKey(end.connectionId, end.port);
        const anchor = measureWireAnchor(element, `connection:${key}`, end.port.direction);
        if (anchor) connectionEnds.set(key, anchor);
    }
    for (const element of document.querySelectorAll<HTMLElement>("[data-port-endpoint]")) {
        const port = JSON.parse(element.dataset.portEndpoint!) as GraphPortEndpoint;
        const key = portEndpointKey(port);
        const anchor = measureWireAnchor(element, `port:${key}`, port.direction);
        if (anchor) exact.set(key, anchor);
    }
    for (const element of document.querySelectorAll<HTMLElement>("[data-port-slot]")) {
        const direction = element.closest<HTMLElement>("[data-direction]")?.dataset.direction;
        if (direction !== "input" && direction !== "output") continue;
        const key = element.dataset.portSlot!;
        const anchor = measureWireAnchor(element, `slot:${key}`, direction);
        if (anchor) slots.set(key, anchor);
    }
    const anchorFor = (port: GraphPortEndpoint, connectionId: string) =>
        connectionEnds.get(connectionEndKey(connectionId, port))
        ?? exact.get(portEndpointKey(port)) ?? slots.get(portSlotKey(port.address, port.direction)) ?? null;
    const timeline = surface.value.querySelector(".timeline-scope");
    const viewport = (timeline ?? surface.value).getBoundingClientRect();
    const heading = timeline?.querySelector(".timeline-heading")?.getBoundingClientRect();
    const top = heading ? Math.max(viewport.top, heading.bottom) : viewport.top;
    selectedWires.value = selectedLaneConnections(scope, selectedLane.value, anchorFor, revealedEnds, { top, bottom: viewport.bottom });
    wireOcclusions.value = measureWireOcclusions(top, viewport.bottom);
    const drag = connectionDrag.value;
    connectionStartAnchor.value = drag && !drag.edit ? anchorFor(drag.port, "") : null;
    previewColorColumns.value = drag ? measurePortColorColumns() : [];
    const end = connectionDrag.value?.edit;
    const anchors = end ? fixedConnectionPorts(scope, end).map((port): PreviewAnchor | null => {
        const anchor = anchorFor(port, end.connection.id);
        return anchor ? { ...anchor, direction: port.direction } : null;
    }).filter((anchor): anchor is PreviewAnchor => !!anchor) : [];
    dragAnchors.value = [...new Map(anchors.map((anchor) => [anchor.id, anchor])).values()];
    wireClips.value = [{ x: viewport.left, y: top, width: viewport.width, height: Math.max(0, viewport.bottom - top) },
        ...[...document.querySelectorAll(".port-panel")].map((element) => {
            const rect = element.getBoundingClientRect();
            return { x: rect.left, y: rect.top, width: rect.width, height: rect.height };
        })];
    updateDropPreview();
}
function scheduleWireUpdate() {
    if (disposed || wireFrame) return;
    void nextTick(() => {
        if (disposed || wireFrame) return;
        wireFrame = requestAnimationFrame(() => { wireFrame = 0; if (!disposed) updateWires(); });
    });
}
watch([selectedLane, () => state.value.scope, () => layout.value.timeline.laneHeight], scheduleWireUpdate, { flush: "post" });
function startConnection(event: PointerEvent, port: GraphPortEndpoint, edit?: GraphConnectionEnd) {
    if (!surface.value || !state.value.scope?.editable || connectionDrag.value) return;
    const handle = event.currentTarget;
    if (!(handle instanceof HTMLElement)) return;
    const anchor = measureWireAnchor(handle, `port:${portEndpointKey(port)}`, port.direction);
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
    sourceGroupToLeave = { nodeKey: addressKey(port.address), direction: port.direction };
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
    const anchor = handle && drag && target && canDropPortConnection(state.value.scope, drag, target)
        ? measureWireAnchor(handle, "drop-preview", target.direction) : null;
    dropPreviewAnchor.value = anchor && target ? { ...anchor, direction: target.direction } : null;
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
        if (hovered?.nodeKey === sourceGroupToLeave.nodeKey && hovered.direction === sourceGroupToLeave.direction) {
            hoveredPortGroup.value = null;
            return;
        }
        sourceGroupToLeave = null;
    }
    if (hovered?.nodeKey !== hoveredPortGroup.value?.nodeKey || hovered?.direction !== hoveredPortGroup.value?.direction) {
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
    if (target && !canDropPortConnection(state.value.scope, drag, target)) {
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
    if (event.type === "lostpointercapture" && surface.value?.hasPointerCapture(event.pointerId)) return;
    if (manualPointer.suppressPointerCancellation()) return;
    releaseManual(event);
    if (connectionDrag.value?.pointerId === event.pointerId) cancelConnection();
}
function pointerLeftWindow(event: PointerEvent) {
    if (manualGestures.pointerControl()) { event.stopImmediatePropagation(); return; }
    if (!event.relatedTarget && (event.clientX <= 0 || event.clientY <= 0
        || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight)) pointerCancelled(event);
}
function focusSurfaceFromPointer(event: PointerEvent) {
    if (event.target instanceof Element
        && !event.target.closest("input, textarea, select, button, a, summary, [contenteditable], [tabindex]:not(.graph-editor)")) {
        surface.value?.focus({ preventScroll: true });
    }
}
function focusIdleSurface() {
    if (document.hasFocus() && document.activeElement === document.body) surface.value?.focus({ preventScroll: true });
}
function keyDown(event: KeyboardEvent) {
    const editingText = event.target instanceof Element
        && !!event.target.closest("input, textarea, select, [contenteditable]:not([contenteditable=false]), [role=textbox]");
    if (!editingText && event.code === "Space" && !event.ctrlKey && !event.metaKey && !event.altKey) {
        event.preventDefault();
        if (!event.repeat) webviewApi.postMessage({ type: "playbackToggle" });
        return;
    }
    const history = projectHistoryShortcut(event, editingText);
    if (history) {
        event.preventDefault();
        event.stopPropagation();
        cancelConnection();
        webviewApi.postMessage({ type: "projectHistory", direction: history });
        return;
    }
    hoverShift.value = keyboardPortHoverShift(hoverShift.value, event.shiftKey);
    if (event.key === "Escape") { cancelConnection(); knobClicks.cancel(); manualPointer.cancel(); }
}
function keyUp(event: KeyboardEvent) {
    hoverShift.value = keyboardPortHoverShift(hoverShift.value, event.shiftKey);
}
function observePointerShift(event: PointerEvent) {
    hoverShift.value = pointerPortHoverShift(hoverShift.value, event);
}
function windowBlurred() {
    if (document.hasFocus()) return;
    hoverShift.value = initialPortHoverShift();
    if (!manualGestures.pointerControl()) blurManual();
    publishInteractionHint();
    // Keep the captured gesture. Native activation can blur this iframe again
    // immediately after a successful pointerdown; the host reports real exits.
}
function nativeWindowFocusChanged(focused: boolean) {
    if (!focused) {
        hoverShift.value = initialPortHoverShift();
        cancelConnection();
        blurManual();
    }
    if (focused) focusManualHint();
    else publishInteractionHint();
}
function createPackage() {
    webviewApi.postMessage({ type: "createGraphPackage", name: packageName.value });
    creatingPackage.value = false;
}
function receive(event: MessageEvent<GraphEditorHostMessage>) {
    if (event.data?.type === "playbackUpdate") {
        playback.receive(event.data.snapshot, event.data.requestedPosition);
        return;
    }
    if (event.data?.type === "observationVisibility") {
        observations.setVisible(event.data.visible);
        return;
    }
    if (event.data?.type === "observationResponse" || event.data?.type === "observationUpdate") {
        observations.receive(event.data);
        return;
    }
    if (event.data?.type === "manualControlState") {
        manualGestures.receive(event.data.interactions, event.data.live);
        if (!manualGestures.pointerControl()) manualPointer.cancel();
        return;
    }
    if (event.data?.type === "graphAction") {
        cancelConnection();
        blurManual();
        const action = event.data;
        if (action.action === "addNode") editing.value = "";
        if (action.action === "editNode" && action.instanceId
            && state.value.scope?.nodes.some((node) => node.instance.instanceId === action.instanceId)) {
            editing.value = action.instanceId;
        }
        if (action.action === "createPackage") { packageName.value = ""; creatingPackage.value = true; }
        return;
    }
    if (event.data?.type !== "graphState") return;
    state.value = event.data.state;
    observations.configuredGeneration(state.value.scope?.execution?.phase === "configurationFailed"
        ? null : state.value.scope?.generation ?? null);
    if (state.value.sessionStatus === "running") observations.refresh();
    else observations.disconnect();
    const entry = manualGestures.numericEntry();
    const entryControl = entry && resolveManualControl(state.value.scope, entry.control);
    if (entry && (!entryControl?.available || !entryControl.enabled)) manualGestures.cancelEntry();
    if (state.value.sessionStatus !== "running") manualGestures.disconnected();
    cancelConnection(); // A new authoritative scope can change port identities.
    if (selectedLane.value && state.value.scope
        && !state.value.scope.nodes.some((node) => addressKey(node.address) === selectedLane.value)) selectedLane.value = null;
    if (editing.value && !state.value.scope?.nodes.some((entry) => entry.instance.instanceId === editing.value)) {
        editing.value = null;
    }
    if (!initializedLayout) {
        layout.value = readLayout(state.value.layout);
        initializedLayout = true;
    }
}
onMounted(() => {
    // Give VS Code a persisted panel state so its serializer can restore
    // this editor. Geometry itself lives in the project layout file.
    webviewApi.setState({ ...webviewApi.getState(), surface: "graphEditor" });
    window.addEventListener("message", receive);
    window.addEventListener("pointermove", moveConnection, true);
    window.addEventListener("pointermove", moveManual, true);
    window.addEventListener("pointerup", releaseManual, true);
    window.addEventListener("mousemove", lockedManualMove, true);
    window.addEventListener("mouseup", lockedManualRelease, true);
    document.addEventListener("pointerlockchange", manualLockChanged);
    document.addEventListener("pointerlockerror", manualLockFailed);
    window.addEventListener("pointerdown", clickOutsideManual, true);
    window.addEventListener("wheel", wheelManual, { capture: true, passive: false });
    window.addEventListener("focus", focusManualHint);
    window.addEventListener("focus", focusIdleSurface);
    window.addEventListener("pointerdown", observePointerShift, true);
    window.addEventListener("pointerup", releaseConnection, true);
    window.addEventListener("pointercancel", pointerCancelled, true);
    window.addEventListener("pointerout", pointerLeftWindow, true);
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
    observations.dispose();
    disposed = true;
    blurManual();
    for (const type of pointerLockExitTypes) window.removeEventListener(type, guardPointerLockExit, true);
    trustedHover.dispose();
    webviewApi.postMessage({ type: "graphInteractionHint", hint: null });
    cancelAnimationFrame(wireFrame);
    wireResizeObserver?.disconnect();
    window.removeEventListener("scroll", scheduleWireUpdate, true);
    window.removeEventListener("message", receive);
    cancelConnection();
    window.removeEventListener("pointermove", moveConnection, true);
    window.removeEventListener("pointermove", moveManual, true);
    window.removeEventListener("pointerup", releaseManual, true);
    window.removeEventListener("mousemove", lockedManualMove, true);
    window.removeEventListener("mouseup", lockedManualRelease, true);
    document.removeEventListener("pointerlockchange", manualLockChanged);
    document.removeEventListener("pointerlockerror", manualLockFailed);
    window.removeEventListener("pointerdown", clickOutsideManual, true);
    window.removeEventListener("wheel", wheelManual, true);
    window.removeEventListener("focus", focusManualHint);
    window.removeEventListener("focus", focusIdleSurface);
    window.removeEventListener("pointerdown", observePointerShift, true);
    window.removeEventListener("pointerup", releaseConnection, true);
    window.removeEventListener("pointercancel", pointerCancelled, true);
    window.removeEventListener("pointerout", pointerLeftWindow, true);
    window.removeEventListener("keydown", keyDown);
    window.removeEventListener("keyup", keyUp);
    stopListeningForFocus?.();
});
</script>

<template>
    <main ref="surface" class="graph-editor" tabindex="0" @pointerdown.capture="focusSurfaceFromPointer"
        @lostpointercapture="pointerCancelled">
        <div v-if="state.sessionStatus !== 'running'" class="status" role="status">
            {{ state.sessionStatus === 'starting' ? 'Starting Intravenous…' : 'Restarting Intravenous…' }}
        </div>
        <div v-if="state.error" class="error" role="alert">{{ state.error }}</div>
        <PresentationHost v-if="state.scope" :node="root" view-choice="editable" />
        <ManualValuePopover :gestures="manualGestures" :version="manualVersion" />
        <NodeEditor v-if="editing !== null" :key="editing" :node="editedNode" :definitions="state.definitions"
            @close="editing = null" />
        <Teleport to="body">
            <div v-if="creatingPackage" class="package-dialog-backdrop" @click.self="creatingPackage = false" @keydown.esc.stop="creatingPackage = false">
                <form class="package-dialog" role="dialog" aria-modal="true" aria-label="New package" @submit.prevent="createPackage">
                    <label>Package name<input v-model="packageName" autofocus required /></label>
                    <button type="button" @click="creatingPackage = false">Cancel</button>
                    <button type="submit" :disabled="!packageName.trim()">Create</button>
                </form>
            </div>
        </Teleport>
        <svg v-if="selectedWires.length || connectionDrag?.moved" class="connection-wire" aria-hidden="true">
            <defs>
                <clipPath id="selected-wire-clip" clipPathUnits="userSpaceOnUse">
                    <rect v-for="(clip, index) in wireClips" :key="index" :x="clip.x" :y="clip.y" :width="clip.width" :height="clip.height" />
                </clipPath>
                <mask id="wire-label-mask" maskUnits="userSpaceOnUse" x="0" y="0" width="100%" height="100%">
                    <rect width="100%" height="100%" fill="white" />
                    <rect v-for="(label, index) in wireOcclusions" :key="index"
                        :x="label.x" :y="label.y - 1" :width="label.width" :height="label.height + 2" fill="black" />
                </mask>
                <linearGradient v-for="(wire, index) in selectedWires" :id="`selected-wire-${index}`" :key="wire.key"
                    gradientUnits="userSpaceOnUse" :x1="wire.source.x" :y1="wire.source.y" :x2="wire.target.x" :y2="wire.target.y">
                    <stop offset="0" :style="{ stopColor: wireColor('output', wire.kind) }" />
                    <stop offset="1" :style="{ stopColor: wireColor('input', wire.kind) }" />
                </linearGradient>
                <linearGradient v-for="(wire, index) in previewWires" :id="`preview-wire-${index}`" :key="wire.anchor.id"
                    gradientUnits="userSpaceOnUse" :x1="wire.anchor.x" :y1="wire.anchor.y" :x2="wire.cursor.x" :y2="wire.cursor.y">
                    <stop offset="0" :style="{ stopColor: wireColor(wire.anchor.direction, connectionDrag?.port.kind) }" />
                    <stop offset="1" :style="{ stopColor: cursorWireColor }" />
                </linearGradient>
                <linearGradient v-for="(wire, index) in dropPreviewWires" :id="`drop-preview-wire-${index}`" :key="`drop-${wire.anchor.id}`"
                    gradientUnits="userSpaceOnUse" :x1="wire.anchor.x" :y1="wire.anchor.y" :x2="wire.target.x" :y2="wire.target.y">
                    <stop offset="0" :style="{ stopColor: wireColor(wire.anchor.direction, connectionDrag?.port.kind) }" />
                    <stop offset="1" :style="{ stopColor: wireColor(wire.target.direction, connectionDrag?.port.kind) }" />
                </linearGradient>
            </defs>
            <g clip-path="url(#selected-wire-clip)" mask="url(#wire-label-mask)">
                <path v-for="(wire, index) in selectedWires" :key="wire.key" class="selected-wire"
                    :class="{ context: !wire.emphasized, event: wire.kind === 'event', pending: !wire.applied,
                        moving: connectionDrag?.edit && wire.connectionIds.includes(connectionDrag.edit.connection.id) }"
                    :d="selectedLaneWirePath(wire)" :stroke="`url(#selected-wire-${index})`" />
            </g>
            <g mask="url(#wire-label-mask)">
                <path v-for="(wire, index) in dropPreviewWires" :key="`drop-${wire.anchor.id}`" class="drop-preview-wire"
                    :d="wire.path" :stroke="`url(#drop-preview-wire-${index})`" />
                <path v-for="(wire, index) in previewWires" :key="wire.anchor.id" class="preview-wire"
                    :d="wire.path" :stroke="`url(#preview-wire-${index})`" />
            </g>
        </svg>
    </main>
</template>

<style scoped>
.graph-editor { height: 100vh; display: flex; flex-direction: column; background: var(--vscode-editor-background); }
.graph-editor :deep(> .presentation-host) { flex: 1; min-height: 0; }
.status, .error { padding: 8px; }
.error { color: var(--vscode-errorForeground); }
.graph-editor:focus { outline: none; }
.connection-wire { position: fixed; inset: 0; width: 100vw; height: 100vh; pointer-events: none; z-index: 30; }
.preview-wire { fill: none; stroke-width: 2; }
.drop-preview-wire { fill: none; stroke-width: 2; stroke-opacity: .4; }
.selected-wire { fill: none; stroke-width: 1.5; stroke-opacity: .72; }
.selected-wire.context { stroke-opacity: .19; }
.selected-wire.event { stroke-dasharray: 1 3; stroke-linecap: round; }
.selected-wire.pending { stroke-dasharray: 5 4; }
.selected-wire.moving { stroke-opacity: .2; }
.package-dialog-backdrop { position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; background: #0005; }
.package-dialog { padding: 16px; display: flex; flex-wrap: wrap; gap: 8px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); border: 1px solid var(--vscode-panel-border); }
.package-dialog label { flex-basis: 100%; display: grid; gap: 5px; }
.package-dialog input { padding: 4px; color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border, transparent); }
</style>

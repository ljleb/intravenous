<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch } from "vue";
import type { GraphEditorHostMessage, GraphEditorState } from "../../src/graphEditorProtocol";
import { addressKey, readLayout } from "../../src/graphScope";
import { webviewApi } from "../host";
import PresentationHost from "../presentation/PresentationHost.vue";
import type { SemanticNode } from "../presentation/semanticNode";
import { provideGraphEditor } from "./context";
import NodeEditor from "./NodeEditor.vue";
import { canMoveConnectionEnd, fixedConnectionPorts, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { beginPortConnection, finishPortConnection, movePortConnection, type PortConnectionDrag } from "../../src/portConnectionGesture";
import { connectionEndKey, portEndpointKey, portSlotKey, selectedLaneConnections, selectedLaneWirePath,
    type PortWireAnchor, type RevealedConnectionEnd, type SelectedLaneWire } from "../../src/selectedLaneConnections";

const state = shallowRef<GraphEditorState>({ sessionStatus: "starting", scope: null, layout: readLayout(null), definitions: [], error: "" });
const layout = ref(readLayout(null));
let initializedLayout = false;
const surface = ref<HTMLElement | null>(null);
const connectionDrag = shallowRef<PortConnectionDrag | null>(null);
const hoveredPortGroup = shallowRef<{ nodeKey: string; direction: "input" | "output" } | null>(null);
const selectedLane = ref<string | null>(null);
const selectedWires = shallowRef<SelectedLaneWire[]>([]);
const dragAnchors = shallowRef<PortWireAnchor[]>([]);
const wireClips = shallowRef<Array<{ x: number; y: number; width: number; height: number }>>([]);
let wireFrame = 0;
let disposed = false;
let wireResizeObserver: ResizeObserver | null = null;
const editing = ref<string | null>(null); // Empty string creates an instance.
const creatingPackage = ref(false);
const packageName = ref("");
const editedNode = computed(() => state.value.scope?.nodes.find((entry) => entry.instance.instanceId === editing.value));
const root = computed<SemanticNode>(() => ({
    id: "", instanceId: "", definitionId: "", kind: "Project graph", sourceIdentity: "",
    memberCount: 0, ports: [], members: [], sourceSpans: [], scope: state.value.scope ?? undefined,
}));
function save() { webviewApi.postMessage({ type: "saveGraphLayout", layout: readLayout(layout.value) }); }
provideGraphEditor({ layout, save,
    copyNodeDebugDetails: (address) => webviewApi.postMessage({ type: "copyNodeDebugDetails", nodeKey: addressKey(address) }),
    scope: computed(() => state.value.scope), connectionDrag, hoveredPortGroup, startConnection, cancelConnection,
    selectedLane, selectLane: (address) => { selectedLane.value = addressKey(address); }, portsChanged: scheduleWireUpdate,
});

function measureWireAnchor(element: HTMLElement, id: string): PortWireAnchor | null {
    const circle = element.querySelector<HTMLElement>(".port-circle") ?? element;
    const rect = circle.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    const point = { id, x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
    const body = element.closest(".expansion-body");
    if (body) {
        const clip = body.getBoundingClientRect();
        if (point.y < clip.top || point.y > clip.bottom || point.x < clip.left || point.x > clip.right) return null;
    }
    return point;
}
function updateWires() {
    const scope = state.value.scope;
    if (!scope || !surface.value) { selectedWires.value = []; dragAnchors.value = []; return; }
    if (!selectedLane.value && !connectionDrag.value?.edit && !document.querySelector("[data-connection-end]")) {
        selectedWires.value = []; dragAnchors.value = []; return;
    }
    const exact = new Map<string, PortWireAnchor>();
    const slots = new Map<string, PortWireAnchor>();
    const connectionEnds = new Map<string, PortWireAnchor>();
    const revealedEnds: RevealedConnectionEnd[] = [];
    for (const element of document.querySelectorAll<HTMLElement>("[data-connection-end]")) {
        const end = JSON.parse(element.dataset.connectionEnd!) as RevealedConnectionEnd;
        revealedEnds.push(end);
        const key = connectionEndKey(end.connectionId, end.port);
        const anchor = measureWireAnchor(element, `connection:${key}`);
        if (anchor) connectionEnds.set(key, anchor);
    }
    for (const element of document.querySelectorAll<HTMLElement>("[data-port-endpoint]")) {
        const port = JSON.parse(element.dataset.portEndpoint!) as GraphPortEndpoint;
        const key = portEndpointKey(port);
        const anchor = measureWireAnchor(element, `port:${key}`);
        if (anchor) exact.set(key, anchor);
    }
    for (const element of document.querySelectorAll<HTMLElement>("[data-port-slot]")) {
        const rect = element.getBoundingClientRect();
        if (!rect.width || !rect.height) continue;
        const key = element.dataset.portSlot!;
        slots.set(key, { id: `slot:${key}`, x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 });
    }
    const anchorFor = (port: GraphPortEndpoint, connectionId: string) =>
        connectionEnds.get(connectionEndKey(connectionId, port))
        ?? exact.get(portEndpointKey(port)) ?? slots.get(portSlotKey(port.address, port.direction)) ?? null;
    selectedWires.value = selectedLaneConnections(scope, selectedLane.value, anchorFor, revealedEnds);
    const end = connectionDrag.value?.edit;
    const anchors = end ? fixedConnectionPorts(scope, end).map((port) => anchorFor(port, end.connection.id))
        .filter((anchor): anchor is PortWireAnchor => !!anchor) : [];
    dragAnchors.value = [...new Map(anchors.map((anchor) => [anchor.id, anchor])).values()];
    const timeline = surface.value.querySelector(".timeline-scope");
    const viewport = (timeline ?? surface.value).getBoundingClientRect();
    const heading = timeline?.querySelector(".timeline-heading")?.getBoundingClientRect();
    const top = heading ? Math.max(viewport.top, heading.bottom) : viewport.top;
    wireClips.value = [{ x: viewport.left, y: top, width: viewport.width, height: Math.max(0, viewport.bottom - top) },
        ...[...document.querySelectorAll(".port-panel")].map((element) => {
            const rect = element.getBoundingClientRect();
            return { x: rect.left, y: rect.top, width: rect.width, height: rect.height };
        })];
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
    const drag = beginPortConnection(port, event.pointerId, event.button, { x: event.clientX, y: event.clientY }, edit);
    if (!drag) return;
    event.preventDefault();
    event.stopPropagation();
    connectionDrag.value = drag;
    scheduleWireUpdate();
    surface.value.focus({ preventScroll: true });
    surface.value.setPointerCapture(event.pointerId);
}
function portAt(event: PointerEvent): GraphPortEndpoint | null {
    const handle = document.elementFromPoint(event.clientX, event.clientY)?.closest<HTMLElement>("[data-port-endpoint]");
    if (!handle?.dataset.portEndpoint) return null;
    return JSON.parse(handle.dataset.portEndpoint) as GraphPortEndpoint;
}
function moveConnection(event: PointerEvent) {
    const drag = connectionDrag.value;
    if (!drag || event.pointerId !== drag.pointerId) return;
    event.preventDefault();
    const next = movePortConnection(drag, event.pointerId, event.buttons, { x: event.clientX, y: event.clientY });
    if (!next) { cancelConnection(); return; }
    connectionDrag.value = next;
    const group = document.elementFromPoint(event.clientX, event.clientY)?.closest<HTMLElement>("[data-port-group]");
    const nodeKey = group?.dataset.nodeKey;
    const direction = group?.dataset.portGroup;
    const hovered: { nodeKey: string; direction: "input" | "output" } | null = nodeKey && (direction === "input" || direction === "output")
        ? { nodeKey, direction } : null;
    if (hovered?.nodeKey !== hoveredPortGroup.value?.nodeKey || hovered?.direction !== hoveredPortGroup.value?.direction) {
        hoveredPortGroup.value = hovered;
    }
}
function releaseConnection(event: PointerEvent) {
    const drag = connectionDrag.value;
    if (!drag || event.pointerId !== drag.pointerId) return;
    event.preventDefault();
    if (event.button !== 0) { cancelConnection(); return; }
    const hit = document.elementFromPoint(event.clientX, event.clientY);
    const target = portAt(event);
    if (drag.edit && target && (!state.value.scope || !canMoveConnectionEnd(state.value.scope, drag.edit, target))) {
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
    hoveredPortGroup.value = null;
    dragAnchors.value = [];
    if (pointerId !== undefined && surface.value?.hasPointerCapture(pointerId)) {
        surface.value.releasePointerCapture(pointerId);
    }
}
function pointerCancelled(event: PointerEvent) {
    if (connectionDrag.value?.pointerId === event.pointerId) cancelConnection();
}
function pointerLeftWindow(event: PointerEvent) {
    if (!event.relatedTarget && (event.clientX <= 0 || event.clientY <= 0
        || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight)) pointerCancelled(event);
}
function keyDown(event: KeyboardEvent) {
    if (event.key === "Escape") cancelConnection();
}
function createPackage() {
    webviewApi.postMessage({ type: "createGraphPackage", name: packageName.value });
    creatingPackage.value = false;
}
function receive(event: MessageEvent<GraphEditorHostMessage>) {
    if (event.data?.type === "graphAction") {
        cancelConnection();
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
    window.addEventListener("pointerup", releaseConnection, true);
    window.addEventListener("pointercancel", pointerCancelled, true);
    window.addEventListener("pointerout", pointerLeftWindow, true);
    window.addEventListener("keydown", keyDown);
    window.addEventListener("blur", cancelConnection);
    window.addEventListener("scroll", scheduleWireUpdate, true);
    wireResizeObserver = new ResizeObserver(scheduleWireUpdate);
    if (surface.value) wireResizeObserver.observe(surface.value);
    webviewApi.postMessage({ type: "graphReady" });
});
onBeforeUnmount(() => {
    disposed = true;
    cancelAnimationFrame(wireFrame);
    wireResizeObserver?.disconnect();
    window.removeEventListener("scroll", scheduleWireUpdate, true);
    window.removeEventListener("message", receive);
    cancelConnection();
    window.removeEventListener("pointermove", moveConnection, true);
    window.removeEventListener("pointerup", releaseConnection, true);
    window.removeEventListener("pointercancel", pointerCancelled, true);
    window.removeEventListener("pointerout", pointerLeftWindow, true);
    window.removeEventListener("keydown", keyDown);
    window.removeEventListener("blur", cancelConnection);
});
</script>

<template>
    <main ref="surface" class="graph-editor" tabindex="-1" @lostpointercapture="pointerCancelled">
        <div v-if="state.sessionStatus !== 'running'" class="status" role="status">
            {{ state.sessionStatus === 'starting' ? 'Starting Intravenous…' : 'Restarting Intravenous…' }}
        </div>
        <div v-if="state.error" class="error" role="alert">{{ state.error }}</div>
        <PresentationHost v-if="state.scope" :node="root" view-choice="editable" />
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
                <linearGradient v-for="(wire, index) in selectedWires" :id="`selected-wire-${index}`" :key="wire.key"
                    gradientUnits="userSpaceOnUse" :x1="wire.source.x" :y1="wire.source.y" :x2="wire.target.x" :y2="wire.target.y">
                    <stop offset="0" class="wire-output" /><stop offset="1" class="wire-input" />
                </linearGradient>
            </defs>
            <g clip-path="url(#selected-wire-clip)">
                <path v-for="(wire, index) in selectedWires" :key="wire.key" class="selected-wire"
                    :class="{ pending: !wire.applied, moving: connectionDrag?.edit && wire.connectionIds.includes(connectionDrag.edit.connection.id) }"
                    :d="selectedLaneWirePath(wire)" :stroke="`url(#selected-wire-${index})`" />
            </g>
            <line v-if="connectionDrag?.moved && !connectionDrag.edit" :x1="connectionDrag.start.x" :y1="connectionDrag.start.y"
                :x2="connectionDrag.end.x" :y2="connectionDrag.end.y" />
            <template v-if="connectionDrag?.moved && connectionDrag.edit">
                <line v-for="anchor in dragAnchors" :key="anchor.id" :x1="anchor.x" :y1="anchor.y"
                    :x2="connectionDrag.end.x" :y2="connectionDrag.end.y" />
            </template>
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
.connection-wire line { stroke: var(--vscode-focusBorder); stroke-width: 2; }
.selected-wire { fill: none; stroke-width: 1.5; stroke-opacity: .85; }
.selected-wire.pending { stroke-dasharray: 5 4; }
.selected-wire.moving { stroke-opacity: .2; }
.wire-output { stop-color: var(--vscode-terminal-ansiBlue); }
.wire-input { stop-color: var(--vscode-terminal-ansiYellow); }
.package-dialog-backdrop { position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; background: #0005; }
.package-dialog { padding: 16px; display: flex; flex-wrap: wrap; gap: 8px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); border: 1px solid var(--vscode-panel-border); }
.package-dialog label { flex-basis: 100%; display: grid; gap: 5px; }
.package-dialog input { padding: 4px; color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border, transparent); }
</style>

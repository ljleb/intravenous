<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { addressKey, type GraphScopeConnection } from "../../src/graphScope";
import { canMoveConnectionEnd, connectionEndsAtPort, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { useOptionalGraphEditor } from "../graph/context";
import { useNode } from "./nodeContext";
import { portDescription, portTypeSummary } from "./nodeSummary";
import type { SemanticPort, SemanticPortDirection } from "./semanticNode";
import { acceptsPortConnection } from "../../src/portConnectionGesture";
import { portSlotKey } from "../../src/selectedLaneConnections";
import ManualInput from "./ManualInput.vue";
import NodePortRow from "./NodePortRow.vue";
import { useOptionalManualControls } from "./manualControlContext";
import { lanePortLayout } from "../../src/lanePortLayout";

const node = useNode();
const editor = useOptionalGraphEditor();
const manualControls = useOptionalManualControls();
const editingControl = computed(() => {
    void manualControls?.version.value;
    return !!node.value.address && !!manualControls?.gestures.isEditingNode(node.value.address);
});
const hasKnob = (port: SemanticPort) => port.kind === "continuous"
    && !!port.manualControl?.available;
const panel = ref<HTMLElement | null>(null);
const portArea = ref<HTMLElement | null>(null);
const portAreaHeight = ref(22);
const knobSize = computed(() => {
    const height = Math.floor(portAreaHeight.value);
    if (height >= 28) return Math.min(height, 31);
    // Keep the compact row visible at its previous 22px cutoff.
    return height >= 22 ? Math.min(height, 24) : 24;
});
const portHeight = (port: SemanticPort) => hasKnob(port) ? knobSize.value : 22;
let portAreaObserver: ResizeObserver | null = null;
const expanded = ref<SemanticPortDirection | null>(null);
const chooserPort = ref<SemanticPort | null>(null);
const hoveredPort = ref<SemanticPort | null>(null);
let hoveredPortRect: DOMRect | null = null;
let popoverPointer: { x: number; y: number } | null = null;
let panelPositionVersion = 0;
const panelPositioned = ref(false);
const panelPosition = ref<Record<string, string>>({ left: "0px", top: "0px" });
const above = ref(false);
const continuation = ref(false);
const groups: Partial<Record<SemanticPortDirection, HTMLElement>> = {};
const nodeKey = computed(() => node.value.address ? addressKey(node.value.address) : "");
const directions: SemanticPortDirection[] = ["input", "output"];
const ports = (direction: SemanticPortDirection) => node.value.ports.filter((port) => port.direction === direction);
const layout = computed(() => ({
    input: lanePortLayout(ports("input"), portAreaHeight.value, portHeight),
    output: lanePortLayout(ports("output"), portAreaHeight.value, portHeight),
}));
const overflowSlots: Partial<Record<SemanticPortDirection, HTMLElement>> = {};
const isDirect = (port: SemanticPort) => layout.value[port.direction].direct.some((entry) => portKey(entry) === portKey(port));
const isOverflowAnchor = (port: SemanticPort) => !!layout.value[port.direction].folded.length
    && layout.value[port.direction].direct.at(-1) === port;
const popoverHasKnob = (port: SemanticPort) => hasKnob(port) && !isDirect(port);
const editable = computed(() => !!editor?.scope.value?.editable && !!node.value.address);
const shiftHeld = computed(() => editor?.shiftHeld.value ?? false);
// Pointer coordinates change on every move; port controls only need the source.
const dragPort = computed(() => editor?.connectionDrag.value?.port ?? null);
const dragEdit = computed(() => editor?.connectionDrag.value?.edit);
const destinationDirection = computed(() => !dragPort.value ? null : dragEdit.value
    ? dragPort.value.direction : dragPort.value.direction === "input" ? "output" : "input");
const choices = computed(() => chooserPort.value && editor?.scope.value
    ? connectionEndsAtPort(editor.scope.value, endpoint(chooserPort.value)) : []);
const shownPorts = computed(() => expanded.value ? layout.value[expanded.value].folded : []);
const portKey = (port: SemanticPort) => `${port.direction}:${port.kind}:${port.index}`;
const linksByPort = computed(() => {
    const links = new Map<string, GraphScopeConnection[]>();
    for (const entry of editor?.scope.value?.connections ?? []) {
        for (const port of node.value.ports) {
            if (entry.connection.kind !== (port.kind === "continuous" ? "sample" : "event")) continue;
            const matchers = port.direction === "input" ? entry.connection.inputs : entry.connection.outputs;
            if (!matchers.some((matcher) => matcher.instanceId === node.value.instanceId && !matcher.path?.length
                && (matcher.port.name === undefined || matcher.port.name === port.name)
                && (matcher.port.index === undefined || matcher.port.index === port.index))) continue;
            const key = portKey(port);
            links.set(key, [...(links.get(key) ?? []), entry]);
        }
    }
    return links;
});
const portLinks = (port: SemanticPort) => linksByPort.value.get(portKey(port)) ?? [];
const applied = (port: SemanticPort) => portLinks(port).some((entry) => entry.applied);
const pending = (port: SemanticPort) => portLinks(port).length > 0 && !applied(port);
const groupConnections = computed(() => {
    const summarize = (direction: SemanticPortDirection) => {
        const group = layout.value[direction].folded;
        const connected = group.filter((port) => portLinks(port).length > 0).length;
        return { state: connected === 0 ? "empty" : connected === group.length ? "full" : "partial",
            pending: group.some(pending), connected };
    };
    return { input: summarize("input"), output: summarize("output") };
});
function setGroup(direction: SemanticPortDirection, element: unknown) {
    if (element instanceof HTMLElement) groups[direction] = element;
    else delete groups[direction];
}
function setOverflow(direction: SemanticPortDirection, element: unknown) {
    if (element instanceof HTMLElement) overflowSlots[direction] = element;
    else delete overflowSlots[direction];
}
function expand(direction: SemanticPortDirection, origin?: DOMRect | null) {
    const group = groups[direction];
    if (!group?.isConnected || !ports(direction).length) return;
    const anchor = origin ? null : group.querySelector<HTMLElement>(".control-row[data-folded-ports]");
    if (anchor) {
        const bounds = anchor.getBoundingClientRect();
        const top = bounds.bottom;
        ++panelPositionVersion;
        above.value = false;
        continuation.value = true;
        panelPosition.value = {
            left: `${bounds.left}px`, top: `${top}px`, minWidth: `${bounds.width}px`,
            width: "max-content", maxWidth: `${Math.max(bounds.width, window.innerWidth - bounds.left - 4)}px`,
            maxHeight: `${Math.max(0, window.innerHeight - top - 4)}px`,
        };
        panelPositioned.value = true;
        expanded.value = direction;
        editor?.portsChanged();
        return;
    }
    const rect = origin ?? overflowSlots[direction]?.getBoundingClientRect() ?? null;
    if (!validOrigin(rect)) return;
    const version = ++panelPositionVersion;
    panelPositioned.value = false;
    continuation.value = false;
    above.value = window.innerHeight - rect.top < 180;
    panelPosition.value = {
        left: `${Math.max(4, Math.min(rect.left, window.innerWidth - 32))}px`,
        width: "max-content", maxWidth: `${window.innerWidth - 8}px`,
        ...(above.value ? { bottom: `${Math.max(4, window.innerHeight - rect.bottom)}px` }
            : { top: `${Math.max(4, rect.top)}px` }),
        maxHeight: `${Math.max(40, Math.min(360, above.value ? rect.bottom - 4 : window.innerHeight - rect.top - 4))}px`,
    };
    expanded.value = direction;
    alignFirstRow(rect, version);
}
function alignFirstRow(origin: DOMRect, version: number) {
    // Measure the rendered row, rather than assuming a font size or line height.
    void nextTick(() => {
        if (version !== panelPositionVersion || !panel.value || !expanded.value) return;
        const firstCircle = panel.value.querySelector<HTMLElement>("[data-port-endpoint] .port-circle, [data-connection-end] .port-circle");
        if (!firstCircle) return;
        const circle = firstCircle.getBoundingClientRect();
        const popup = panel.value.getBoundingClientRect();
        if (!firstCircle.isConnected || !validOrigin(circle) || !validOrigin(popup)
            || version !== panelPositionVersion) return;
        const dx = origin.left + origin.width / 2 - circle.left - circle.width / 2;
        const dy = origin.top + origin.height / 2 - circle.top - circle.height / 2;
        const left = Math.max(4, popup.left + dx);
        const width = Math.min(popup.width, Math.max(24, window.innerWidth - left - 4));
        if (above.value) {
            const bottom = window.innerHeight - popup.bottom - dy;
            panelPosition.value = { left: `${left}px`, bottom: `${bottom}px`, width: `${width}px`,
                maxHeight: `${Math.max(24, Math.min(360, window.innerHeight - bottom - 4))}px` };
        } else {
            const top = popup.top + dy;
            panelPosition.value = { left: `${left}px`, top: `${top}px`, width: `${width}px`,
                maxHeight: `${Math.max(24, Math.min(360, window.innerHeight - top - 4))}px` };
        }
        panelPositioned.value = true;
        editor?.portsChanged();
        // The pointer may have left while layout events were being deferred.
        // Wait for the positioned style to reach the DOM before hit-testing.
        void nextTick(() => {
            if (version === panelPositionVersion) checkPopoverPointer();
        });
    });
}
function validOrigin(rect: DOMRect | null): rect is DOMRect {
    return !!rect && rect.width > 0 && rect.height > 0
        && Number.isFinite(rect.left) && Number.isFinite(rect.top);
}
function hoverGroup(direction: SemanticPortDirection) {
    if (chooserPort.value && !dragPort.value) return;
    if (!dragPort.value) {
        if (layout.value[direction].folded.length) {
            expand(direction);
        }
        return;
    }
    // During a captured drag, GraphEditor owns hover hit-testing. A layout
    // boundary event must not reopen the popover that the press just closed.
    const hover = editor?.hoveredPortGroup.value;
    if (!hover || hover.nodeKey !== nodeKey.value || hover.direction !== direction) return;
    if (direction !== destinationDirection.value || !groupAccepts(direction)) {
        closePanel();
        return;
    }
    // Do not cover a direct handle with a port list.
    if (layout.value[direction].folded.length) {
        expand(direction);
    }
    else closePanel();
}
function leave() {
    // Boundary events only request a check. A row replacement must not
    // overwrite the pointer position recorded by actual movement.
    checkPopoverPointer();
}
function checkPopoverPointer() {
    if (!expanded.value || !panelPositioned.value || !popoverPointer || dragPort.value || editingControl.value) return;
    const current = document.elementFromPoint(popoverPointer.x, popoverPointer.y);
    const group = groups[expanded.value];
    if (current && (group?.contains(current) || panel.value?.contains(current))) return;
    closePanel();
}
function movePopoverPointer(event: PointerEvent) {
    if (editingControl.value) return;
    popoverPointer = { x: event.clientX, y: event.clientY };
    checkPopoverPointer();
}
function leavePopoverWindow(event: PointerEvent) {
    if (editingControl.value || document.pointerLockElement) return;
    if (!event.relatedTarget && (event.clientX <= 0 || event.clientY <= 0
        || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight)) closePanel();
}
function closePanel() {
    ++panelPositionVersion;
    panelPositioned.value = false;
    expanded.value = null; chooserPort.value = null;
    hoveredPort.value = null; hoveredPortRect = null;
}
function closeConnectionChooser() {
    const port = chooserPort.value;
    if (!port || editingControl.value) return;
    ++panelPositionVersion;
    chooserPort.value = null;
    if (isDirect(port) && !isOverflowAnchor(port)) closePanel();
    else expand(port.direction);
}
function endpoint(port: SemanticPort): GraphPortEndpoint {
    return { address: node.value.address!, direction: port.direction,
        kind: port.kind === "continuous" ? "sample" : "event", index: port.index, name: port.name };
}
function openConnectionChooser(port: SemanticPort, origin = hoveredPortRect) {
    if (!validOrigin(origin) || !editable.value || dragPort.value || !editor?.scope.value
        || (chooserPort.value && portKey(chooserPort.value) === portKey(port))
        || connectionEndsAtPort(editor.scope.value, endpoint(port)).length < 1) return;
    chooserPort.value = port;
    expand(port.direction, origin);
}
function portIconRect(event: Event): DOMRect | null {
    const handle = event.currentTarget;
    if (!(handle instanceof HTMLElement) || !handle.isConnected || handle.closest(".port-panel.positioning")) return null;
    const rect = (handle.querySelector<HTMLElement>(".port-circle") ?? handle).getBoundingClientRect();
    return validOrigin(rect) ? rect : null;
}
function enterPort(event: Event, port: SemanticPort) {
    const rect = portIconRect(event);
    if (!rect) return;
    hoveredPort.value = port;
    hoveredPortRect = rect;
    if (shiftHeld.value) openConnectionChooser(port, hoveredPortRect);
    else if (!dragPort.value && !editingControl.value
        && expanded.value && !isOverflowAnchor(port)
        && event.currentTarget instanceof Element && !event.currentTarget.closest(".port-panel")) {
        closePanel();
        hoveredPort.value = port;
        hoveredPortRect = rect;
    }
}
function start(event: PointerEvent, port: SemanticPort) {
    if (!editable.value || event.button !== 0 || !editor?.scope.value) return;
    if (!event.shiftKey) { editor.startConnection(event, endpoint(port)); return; }
    event.preventDefault(); event.stopPropagation();
    const ends = connectionEndsAtPort(editor.scope.value, endpoint(port));
    if (ends.length === 1) editor.startConnection(event, endpoint(port), ends[0]);
    else if (ends.length > 1) openConnectionChooser(port, portIconRect(event));
}
function pickConnection(event: PointerEvent, end: GraphConnectionEnd) {
    if (event.button !== 0) return;
    editor?.startConnection(event, end.port, end);
}
function accepts(port: SemanticPort) {
    if (!editable.value || !dragPort.value) return false;
    return dragEdit.value ? !!editor?.scope.value && canMoveConnectionEnd(editor.scope.value, dragEdit.value, endpoint(port))
        : acceptsPortConnection(dragPort.value, endpoint(port));
}
function groupAccepts(direction: SemanticPortDirection) {
    return layout.value[direction].folded.some(accepts);
}
function connectionLabel(connection: GraphScopeConnection["connection"]) {
    const label = (matcher: typeof connection.outputs[number]) => {
        const instance = editor?.scope.value?.nodes.find((entry) => entry.address.instanceId === matcher.instanceId)?.instance;
        const name = instance?.displayName || instance?.definitionId.split(".").pop() || "Unavailable node";
        return `${name}.${matcher.port.name || (matcher.port.index === undefined ? "*" : matcher.port.index + 1)}`;
    };
    return `${connection.outputs.map(label).join(", ")} → ${connection.inputs.map(label).join(", ")}`;
}
function portStatus(port: SemanticPort) {
    const links = portLinks(port);
    const pending = links.filter((entry) => !entry.applied).length;
    const first = links[0]?.connection;
    const route = first && links.length === 1 && first.outputs.length === 1 && first.inputs.length === 1
        ? connectionLabel(first)
        : links.length ? `${links.length} ${links.length === 1 ? "connection" : "connections"}` : "";
    const name = `${port.direction === "input" ? "In" : "Out"} ${port.name || port.index + 1}`;
    return [name, route, pending ? `${pending} pending` : "", portTypeSummary(port)].filter(Boolean).join(" · ");
}
watch(() => editor?.hoveredPortGroup.value, (group) => {
    if (!dragPort.value) return;
    if (group?.nodeKey === nodeKey.value) hoverGroup(group.direction);
    else closePanel();
});
watch(dragPort, closePanel);
watch(expanded, (direction, previous) => {
    if (direction && !previous) {
        popoverPointer = null;
        window.addEventListener("pointermove", movePopoverPointer, true);
        window.addEventListener("pointerout", leavePopoverWindow, true);
    } else if (!direction) {
        window.removeEventListener("pointermove", movePopoverPointer, true);
        window.removeEventListener("pointerout", leavePopoverWindow, true);
        popoverPointer = null;
    }
}, { flush: "sync" });
// Manual-value publications replace the scope object without changing port
// identities. Keep the control under the pointer across those acknowledgements.
watch(() => JSON.stringify(node.value.ports.map((port) => [port.direction, port.kind, port.index, port.name])), closePanel);
watch(editingControl, (editing) => {
    if (editing) return;
    if (!document.hasFocus()) closePanel();
    else void nextTick(checkPopoverPointer);
});
watch(choices, (entries) => { if (chooserPort.value && !entries.length) closeConnectionChooser(); });
watch(shiftHeld, (held) => {
    if (!held) closeConnectionChooser();
    else if (hoveredPort.value) openConnectionChooser(hoveredPort.value);
});
watch([expanded, chooserPort], () => editor?.portsChanged(), { flush: "post" });
watch(() => JSON.stringify(directions.map((direction) => layout.value[direction].direct.map(portKey))),
    () => { closePanel(); editor?.portsChanged(); }, { flush: "post" });
function viewportChanged(event: Event) {
    if (editingControl.value) return;
    if (event.target instanceof Node && panel.value?.contains(event.target)) return;
    closePanel();
}
function focusContinuation(direction: SemanticPortDirection) {
    hoverGroup(direction);
    void nextTick(async () => {
        await nextTick();
        panel.value?.querySelector<HTMLButtonElement>(".port-handle")?.focus({ preventScroll: true });
    });
}
function dismissPanel(event: KeyboardEvent) {
    if (event.key === "Escape" && !editingControl.value) closePanel();
}
function windowBlurred() { if (!editingControl.value) closePanel(); }
function leaveFocus() {
    queueMicrotask(() => {
        const direction = expanded.value;
        const focused = document.activeElement;
        if (!direction || editingControl.value || dragPort.value
            || groups[direction]?.contains(focused) || panel.value?.contains(focused)) return;
        if (popoverPointer) checkPopoverPointer();
        else closePanel();
    });
}
onMounted(() => {
    portAreaObserver = new ResizeObserver(([entry]) => {
        if (entry) portAreaHeight.value = entry.contentRect.height;
    });
    if (portArea.value) portAreaObserver.observe(portArea.value);
    editor?.portsChanged();
    window.addEventListener("resize", viewportChanged);
    window.addEventListener("scroll", viewportChanged, true);
    window.addEventListener("keydown", dismissPanel);
    window.addEventListener("blur", windowBlurred);
});
onBeforeUnmount(() => {
    portAreaObserver?.disconnect();
    window.removeEventListener("pointermove", movePopoverPointer, true);
    window.removeEventListener("pointerout", leavePopoverWindow, true);
    editor?.portsChanged();
    window.removeEventListener("resize", viewportChanged);
    window.removeEventListener("scroll", viewportChanged, true);
    window.removeEventListener("keydown", dismissPanel);
    window.removeEventListener("blur", windowBlurred);
    if (dragPort.value && addressKey(dragPort.value.address) === nodeKey.value) editor?.cancelConnection();
});
</script>

<template>
    <div ref="portArea" class="node-ports" @iv-hoverleave="leave" @focusout="leaveFocus">
        <div v-for="direction in directions" :key="direction" :ref="(element) => setGroup(direction, element)"
            class="port-group" :data-port-group="direction" :data-node-key="nodeKey" :data-direction="direction">
            <NodePortRow v-for="port in layout[direction].direct" :key="portKey(port)" :port="port" :endpoint="endpoint(port)"
                :height="portHeight(port)" :knob-size="knobSize" :has-knob="hasKnob(port)" :hint="portStatus(port)"
                :connected="applied(port)" :pending="pending(port)" :unavailable="destinationDirection === port.direction && !accepts(port)"
                :overflow-slot="isOverflowAnchor(port) && node.address ? portSlotKey(node.address, direction) : undefined"
                :expanded="expanded === direction && !chooserPort"
                @handle-hover="enterPort($event, port)" @handle-leave="hoveredPort = null"
                @wire-start="start($event, port)" @overflow="hoverGroup(direction)" @focus-overflow="focusContinuation(direction)" />
            <div v-if="layout[direction].folded.length && !layout[direction].direct.length" :ref="(element) => setOverflow(direction, element)"
                class="port-slot" role="button" tabindex="0" :data-folded-ports="direction"
                :class="{ single: layout[direction].folded.length === 1,
                    pending: groupConnections[direction].pending, unavailable: destinationDirection === direction && !groupAccepts(direction) }"
                :data-port-kind="layout[direction].folded.every((port) => port.kind === 'event') ? 'event' : 'sample'"
                :data-connection-state="groupConnections[direction].state"
                :data-port-slot="node.address ? portSlotKey(node.address, direction) : undefined"
                :aria-label="`${layout[direction].folded.length} more ${direction} ports; ${groupConnections[direction].connected} connected`" :aria-expanded="expanded === direction && !chooserPort"
                :aria-disabled="destinationDirection === direction && !groupAccepts(direction)"
                :data-status-hint="`${layout[direction].folded.length} more ${direction} ports · ${groupConnections[direction].connected} connected`"
                @iv-hoverenter="hoverGroup(direction)" @focus="hoverGroup(direction)" @keydown.enter="hoverGroup(direction)"
                @keydown.down.prevent="focusContinuation(direction)">
                <span class="port-circle stacked-first" />
                <span v-if="layout[direction].folded.length > 1" class="port-circle stacked-second" />
            </div>
        </div>
    </div>
    <Teleport to="body">
        <div v-if="expanded" ref="panel" class="port-panel" :class="{ above, continuation, positioning: !panelPositioned }" :style="panelPosition"
            :aria-hidden="!panelPositioned"
            :data-port-group="expanded" :data-node-key="nodeKey" :data-direction="expanded"
            @iv-hoverleave="leave" @focusout="leaveFocus" @keydown.esc="closePanel">
            <div class="expansion-body">
                <template v-if="chooserPort">
                    <div v-for="(end, index) in choices" :key="end.connection.id" class="port-row"
                        :data-direction="end.port.direction"
                        :class="{ 'with-knob': index === 0 && popoverHasKnob(chooserPort) }">
                        <button type="button" class="port-entry connection-choice"
                            :data-port-kind="end.port.kind"
                            :data-connection-end="JSON.stringify({ connectionId: end.connection.id, port: end.port })"
                            :class="{ connected: end.connection.applied, pending: !end.connection.applied }"
                            :data-status-hint="`${end.connection.applied ? '' : 'Pending · '}${connectionLabel(end.connection.connection)}`"
                            :aria-description="`${chooserPort.name} · ${portDescription(chooserPort)}`"
                            @pointerdown="pickConnection($event, end)">
                            <span class="port-circle" /><span class="port-name">{{ connectionLabel(end.connection.connection) }}</span>
                        </button>
                        <ManualInput v-if="index === 0 && popoverHasKnob(chooserPort)" :name="chooserPort.name" :direction="chooserPort.direction" />
                    </div>
                </template>
                <template v-else>
                    <NodePortRow v-for="port in shownPorts" :key="portKey(port)" :port="port" :endpoint="endpoint(port)"
                        :height="portHeight(port)" :knob-size="knobSize" :has-knob="hasKnob(port)" :hint="portStatus(port)"
                        :connected="applied(port)" :pending="pending(port)" :unavailable="destinationDirection === port.direction && !accepts(port)"
                        continuation @handle-hover="enterPort($event, port)" @handle-leave="hoveredPort = null" @wire-start="start($event, port)" />
                </template>
            </div>
        </div>
    </Teleport>
</template>

<style scoped>
.node-ports { display: grid; grid-template-columns: minmax(0, 1fr) max-content; align-items: start; gap: 4px; margin-top: 5px; min-height: 22px; }
.port-group { display: flex; flex-direction: column; gap: 3px; min-height: var(--port-slot-height, 22px); }
[data-direction="input"] { --port-base-color: var(--iv-sample-input); --port-event-color: var(--iv-event-input); --port-color: var(--port-base-color); }
[data-direction="output"] { --port-base-color: var(--iv-sample-output); --port-event-color: var(--iv-event-output); --port-color: var(--port-base-color); }
[data-port-kind="event"] { --port-color: var(--port-event-color); }
.port-circle { display: inline-block; flex: 0 0 12px; width: 12px; height: 12px; box-sizing: border-box; border: 2px solid var(--port-color); border-radius: 50%; }
[data-port-kind="event"]:not(.single) .port-circle { border-radius: 3px; }
.connected .port-circle { background: var(--port-color); }
.pending .port-circle { border-style: dashed; }
.port-slot { position: relative; flex: 0 0 var(--port-slot-height, 22px); width: 28px; height: var(--port-slot-height, 22px);
    box-sizing: border-box; border: 1px solid transparent; border-radius: 7px; }
.port-slot .port-circle { width: min(12px, calc(var(--port-slot-height, 22px) - 2px)); height: auto; aspect-ratio: 1; }
.stacked-first { position: absolute; left: 4px; top: 50%; transform: translateY(-50%); }
.stacked-second { position: absolute; left: 11px; top: 50%; transform: translateY(-50%); background: var(--vscode-editor-background); }
.single .stacked-first { left: 7px; }
.port-slot[data-connection-state="partial"] .stacked-second,
.port-slot[data-connection-state="full"] .port-circle { background: var(--port-color); }
.port-slot[data-hovered] { background: color-mix(in srgb, var(--port-color) 12%, transparent); }
.port-slot:focus-visible { outline: 1px solid var(--port-color); }
.port-panel { position: fixed; z-index: 20; display: flex; flex-direction: column; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); box-shadow: 0 3px 12px #0005; }
.port-panel.continuation { border: 0; border-radius: 0 0 6px 6px; box-shadow: 0 1px var(--vscode-panel-border), 0 3px 10px #0005; }
.port-panel.positioning { visibility: hidden; pointer-events: none; }
.expansion-body { display: flex; flex-direction: column; gap: 3px; overflow-y: auto; overflow-x: hidden; min-height: 0; padding: 3px; }
.continuation .expansion-body { padding: 3px 0; }
.above .expansion-body { display: flex; flex-direction: column-reverse; }
.port-row { display: flex; flex-shrink: 0; align-items: center; min-width: 0; }
.port-row .port-entry { flex: 1; min-width: 0; width: auto; }
.port-row.with-knob { --chooser-columns: 24px 52px minmax(0, 1fr); display: grid; grid-template-columns: var(--chooser-columns); }
.port-row.with-knob[data-direction="input"] { --chooser-columns: 36px 24px minmax(0, 1fr); }
.with-knob > .port-entry { grid-column: 1 / -1; grid-row: 1; display: grid;
    grid-template-columns: var(--chooser-columns); gap: 0; width: 100%; box-sizing: border-box; padding-inline: 0 5px; }
.with-knob > .port-entry .port-circle { grid-column: 1; }
.with-knob[data-direction="input"] > .port-entry .port-circle { grid-column: 2; }
.with-knob > .port-entry .port-name { grid-column: 3; }
.with-knob > .manual-input { grid-column: 2; grid-row: 1; z-index: 1; }
.with-knob[data-direction="input"] > .manual-input { grid-column: 1; }
.port-entry { display: flex; flex-shrink: 0; gap: 10px; align-items: center; width: 100%; padding: 5px; border: 0; background: transparent; color: inherit; text-align: left; cursor: crosshair; touch-action: none; }
.port-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.port-entry[data-hovered] { background: var(--vscode-list-hoverBackground); }
.unavailable { --port-color: color-mix(in srgb, var(--port-base-color) 25%, var(--vscode-disabledForeground) 75%); color: var(--vscode-disabledForeground); }
</style>

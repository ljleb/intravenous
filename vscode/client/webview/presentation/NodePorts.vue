<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { addressKey, type GraphScopeConnection } from "../../src/graphScope";
import { canMoveConnectionEnd, connectionEndsAtPort, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { useOptionalGraphEditor } from "../graph/context";
import { useNode } from "./nodeContext";
import { portDescription } from "./nodeSummary";
import type { SemanticPort, SemanticPortDirection } from "./semanticNode";
import { acceptsPortConnection } from "../../src/portConnectionGesture";
import { portSlotKey } from "../../src/selectedLaneConnections";
import ManualInput from "./ManualInput.vue";
import { useOptionalManualControls } from "./manualControlContext";
import { lanePortLayout } from "../../src/lanePortLayout";

const node = useNode();
const editor = useOptionalGraphEditor();
const manualControls = useOptionalManualControls();
const editingControl = computed(() => {
    void manualControls?.version.value;
    return !!node.value.address && !!manualControls?.gestures.isEditingNode(node.value.address);
});
const hasKnob = (port: SemanticPort) => port.direction === "input" && port.kind === "continuous"
    && !!port.manualControl?.available;
const portHeight = (port: SemanticPort) => hasKnob(port) ? 30 : 22;
const panel = ref<HTMLElement | null>(null);
const portArea = ref<HTMLElement | null>(null);
const portAreaHeight = ref(22);
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
    const rect = origin ?? overflowSlots[direction]?.getBoundingClientRect() ?? null;
    if (!validOrigin(rect)) return;
    const version = ++panelPositionVersion;
    panelPositioned.value = false;
    const width = Math.min(340, window.innerWidth - 8);
    above.value = window.innerHeight - rect.top < 180;
    panelPosition.value = {
        left: `${Math.max(4, Math.min(rect.left, window.innerWidth - width - 4))}px`,
        width: `${width}px`,
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
        const firstCircle = panel.value.querySelector<HTMLElement>(".port-entry .port-circle");
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
    if (isDirect(port)) closePanel();
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
function portIconRect(event: PointerEvent): DOMRect | null {
    const handle = event.currentTarget;
    if (!(handle instanceof HTMLElement) || !handle.isConnected || handle.closest(".port-panel.positioning")) return null;
    const rect = (handle.querySelector<HTMLElement>(".port-circle") ?? handle).getBoundingClientRect();
    return validOrigin(rect) ? rect : null;
}
function enterPort(event: PointerEvent, port: SemanticPort) {
    const rect = portIconRect(event);
    if (!rect) return;
    hoveredPort.value = port;
    hoveredPortRect = rect;
    if (shiftHeld.value) openConnectionChooser(port, hoveredPortRect);
    else if (!dragPort.value && !editingControl.value
        && expanded.value && event.currentTarget instanceof Element && !event.currentTarget.closest(".port-panel")) {
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
function portTitle(port: SemanticPort) {
    const details = `${port.direction}: ${port.name || port.index + 1} · ${portDescription(port)}`;
    return [details, ...portLinks(port).map((entry) => `${connectionLabel(entry.connection)}${entry.applied ? "" : " (not applied)"}`)].join("\n");
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
function dismissPanel(event: KeyboardEvent) {
    if (event.key === "Escape" && !editingControl.value) closePanel();
}
function windowBlurred() { if (!editingControl.value) closePanel(); }
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
    <div ref="portArea" class="node-ports" @mouseleave="leave">
        <div v-for="direction in directions" :key="direction" :ref="(element) => setGroup(direction, element)"
            class="port-group" :data-port-group="direction" :data-node-key="nodeKey" :data-direction="direction">
            <div v-for="port in layout[direction].direct" :key="`${port.kind}:${port.index}`"
                class="direct-port-row" :style="{ height: `${portHeight(port)}px` }">
                <button type="button" class="port-handle"
                    :class="{ connected: applied(port), pending: pending(port), unavailable: destinationDirection === port.direction && !accepts(port) }"
                    :aria-disabled="destinationDirection === port.direction && !accepts(port)"
                    :data-port-endpoint="JSON.stringify(endpoint(port))"
                    :title="portTitle(port)"
                    :aria-label="`${port.direction} ${port.name || port.index + 1}`"
                    @pointerenter="enterPort($event, port)" @pointermove="enterPort($event, port)" @pointerleave="hoveredPort = null"
                    @pointerdown="start($event, port)">
                    <span class="port-circle" />
                </button>
                <ManualInput v-if="hasKnob(port)" :name="port.name" />
                <span v-if="direction === 'input'" class="direct-port-name" :title="port.name">{{ port.name || port.index + 1 }}</span>
            </div>
            <div v-if="layout[direction].folded.length" :ref="(element) => setOverflow(direction, element)"
                class="port-slot" role="button" tabindex="0" :data-folded-ports="direction"
                :class="{ pending: groupConnections[direction].pending, unavailable: destinationDirection === direction && !groupAccepts(direction) }"
                :data-connection-state="groupConnections[direction].state"
                :data-port-slot="node.address ? portSlotKey(node.address, direction) : undefined"
                :aria-label="`${layout[direction].folded.length} more ${direction} ports; ${groupConnections[direction].connected} connected`" :aria-expanded="expanded === direction && !chooserPort"
                :aria-disabled="destinationDirection === direction && !groupAccepts(direction)"
                :title="`${layout[direction].folded.length} more ${direction} ports; ${groupConnections[direction].connected} connected`"
                @mouseenter="hoverGroup(direction)" @focus="hoverGroup(direction)" @keydown.enter="hoverGroup(direction)">
                <span class="port-circle stacked-first" /><span class="port-circle stacked-second" />
            </div>
        </div>
    </div>
    <Teleport to="body">
        <div v-if="expanded" ref="panel" class="port-panel" :class="{ above, positioning: !panelPositioned }" :style="panelPosition"
            :aria-hidden="!panelPositioned"
            :data-port-group="expanded" :data-node-key="nodeKey" :data-direction="expanded"
            @mouseleave="leave" @keydown.esc="closePanel">
            <div class="expansion-body">
                <template v-if="chooserPort">
                    <div v-for="(end, index) in choices" :key="end.connection.id" class="port-row"
                        :class="{ 'with-knob': index === 0 && popoverHasKnob(chooserPort) }">
                        <button type="button" class="port-entry connection-choice"
                            :data-connection-end="JSON.stringify({ connectionId: end.connection.id, port: end.port })"
                            :class="{ connected: end.connection.applied, pending: !end.connection.applied }"
                            :title="`${chooserPort.name} · ${portDescription(chooserPort)}\n${connectionLabel(end.connection.connection)}`"
                            @pointerdown="pickConnection($event, end)">
                            <span class="port-circle" /><span class="port-name">{{ connectionLabel(end.connection.connection) }}</span>
                        </button>
                        <ManualInput v-if="index === 0 && popoverHasKnob(chooserPort)" :name="chooserPort.name" />
                    </div>
                </template>
                <template v-else>
                    <div v-for="port in shownPorts" :key="`${port.direction}:${port.kind}:${port.index}`" class="port-row"
                        :class="{ 'with-knob': hasKnob(port) }">
                        <button type="button" class="port-entry"
                            :class="{ unavailable: destinationDirection === port.direction && !accepts(port), connected: applied(port), pending: pending(port) }"
                            :aria-disabled="destinationDirection === port.direction && !accepts(port)"
                            :data-port-endpoint="JSON.stringify(endpoint(port))" :title="portTitle(port)"
                            @pointerenter="enterPort($event, port)" @pointermove="enterPort($event, port)" @pointerleave="hoveredPort = null"
                            @pointerdown="start($event, port)">
                            <span class="port-circle" /><span class="port-name">{{ port.name || port.index + 1 }}</span>
                            <small v-if="!hasKnob(port)">{{ portDescription(port) }}</small>
                        </button>
                        <ManualInput v-if="hasKnob(port)" :name="port.name" />
                    </div>
                </template>
            </div>
        </div>
    </Teleport>
</template>

<style scoped>
.node-ports { display: grid; grid-template-columns: minmax(0, 1fr) max-content; align-items: start; gap: 6px; margin-top: 5px; min-height: 22px; container: lane-ports / inline-size; }
.port-group { display: flex; flex-direction: column; gap: 3px; min-height: 22px; }
.direct-port-row { display: flex; align-items: center; gap: 2px; min-width: 0; flex-shrink: 0; }
.direct-port-row .port-handle { flex: 0 0 22px; }
.direct-port-row > .manual-input { box-sizing: border-box; flex: 0 1 auto; min-width: 38px; max-width: 105px; padding: 0 2px; }
.direct-port-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 11px; }
@container lane-ports (max-width: 150px) {
    .direct-port-row :deep(.manual-values) { display: none; }
    .direct-port-name { display: none; }
}
[data-direction="input"] { --port-base-color: var(--vscode-terminal-ansiYellow); --port-color: var(--port-base-color); }
[data-direction="output"] { --port-base-color: var(--vscode-terminal-ansiBlue); --port-color: var(--port-base-color); }
.port-handle { display: grid; place-items: center; padding: 0; height: 22px; width: 22px; border: 0; background: transparent; cursor: crosshair; touch-action: none; }
.port-circle { display: inline-block; flex: 0 0 10px; width: 10px; height: 10px; box-sizing: border-box; border: 2px solid var(--port-color); border-radius: 50%; }
.connected .port-circle { background: var(--port-color); }
.pending .port-circle { border-style: dashed; }
.port-slot { position: relative; flex: 0 0 22px; width: 28px; height: 22px; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; }
.stacked-first { position: absolute; left: 5px; top: 4px; }
.stacked-second { position: absolute; left: 12px; top: 4px; background: var(--vscode-editor-background); }
.port-slot[data-connection-state="partial"] .stacked-second,
.port-slot[data-connection-state="full"] .port-circle { background: var(--port-color); }
.port-handle:focus-visible, .port-slot:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
.port-panel { position: fixed; z-index: 20; display: flex; flex-direction: column; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); box-shadow: 0 3px 12px #0005; }
.port-panel.positioning { visibility: hidden; pointer-events: none; }
.expansion-body { overflow-y: auto; min-height: 0; padding: 3px; }
.above .expansion-body { display: flex; flex-direction: column-reverse; }
.port-row { display: flex; flex-shrink: 0; align-items: center; min-width: 0; }
.port-row .port-entry { flex: 1; min-width: 0; width: auto; }
.port-row.with-knob { display: grid; grid-template-columns: 20px 120px minmax(0, 1fr); }
.with-knob > .port-entry { grid-column: 1 / -1; grid-row: 1; display: grid;
    grid-template-columns: 20px 120px minmax(0, 1fr); gap: 0; width: 100%; box-sizing: border-box; }
.with-knob > .port-entry .port-circle { grid-column: 1; }
.with-knob > .port-entry .port-name { grid-column: 3; }
.with-knob > .manual-input { grid-column: 2; grid-row: 1; z-index: 1; }
.port-entry { display: flex; flex-shrink: 0; gap: 10px; align-items: center; width: 100%; padding: 5px; border: 0; background: transparent; color: inherit; text-align: left; cursor: crosshair; touch-action: none; }
.port-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.port-entry small { max-width: 45%; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-descriptionForeground); text-align: right; }
.port-entry:hover { background: var(--vscode-list-hoverBackground); }
.unavailable { --port-color: color-mix(in srgb, var(--port-base-color) 25%, var(--vscode-disabledForeground) 75%); color: var(--vscode-disabledForeground); }
.unavailable small { color: var(--vscode-disabledForeground); }
</style>

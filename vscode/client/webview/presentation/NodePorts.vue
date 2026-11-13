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

const node = useNode();
const editor = useOptionalGraphEditor();
const area = ref<HTMLElement | null>(null);
const panel = ref<HTMLElement | null>(null);
const expanded = ref<SemanticPortDirection | null>(null);
const chooserPort = ref<SemanticPort | null>(null);
const hoveredPort = ref<SemanticPort | null>(null);
let hoveredPortRect: DOMRect | null = null;
let panelPositionVersion = 0;
const panelPosition = ref<Record<string, string>>({ left: "0px", top: "0px" });
const above = ref(false);
const groups: Partial<Record<SemanticPortDirection, HTMLElement>> = {};
const nodeKey = computed(() => node.value.address ? addressKey(node.value.address) : "");
const directions: SemanticPortDirection[] = ["input", "output"];
const ports = (direction: SemanticPortDirection) => node.value.ports.filter((port) => port.direction === direction);
const editable = computed(() => !!editor?.scope.value?.editable && !!node.value.address);
// Pointer coordinates change on every move; port controls only need the source.
const dragPort = computed(() => editor?.connectionDrag.value?.port ?? null);
const dragEdit = computed(() => editor?.connectionDrag.value?.edit);
const choices = computed(() => chooserPort.value && editor?.scope.value
    ? connectionEndsAtPort(editor.scope.value, endpoint(chooserPort.value)) : []);
const shownPorts = computed(() => expanded.value ? ports(expanded.value) : []);
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
        const group = ports(direction);
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
function expand(direction: SemanticPortDirection, origin?: DOMRect | null) {
    const group = groups[direction];
    if (!group || !ports(direction).length) return;
    const version = ++panelPositionVersion;
    const rect = origin ?? group.getBoundingClientRect();
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
        editor?.portsChanged();
    });
}
function hoverGroup(direction: SemanticPortDirection) {
    if (chooserPort.value && !dragPort.value) return;
    if (!dragPort.value) {
        if (ports(direction).length > 1) expand(direction);
        return;
    }
    // Retain the source group's contents until the pointer reaches a target.
    if (!dragEdit.value && addressKey(dragPort.value.address) === nodeKey.value && direction === dragPort.value.direction) return;
    const targetDirection = dragEdit.value ? dragPort.value.direction : dragPort.value.direction === "input" ? "output" : "input";
    // Do not cover a direct handle with a port list.
    if (ports(targetDirection).length > 1) expand(targetDirection);
    else expanded.value = null;
}
function leave(event: MouseEvent) {
    const target = event.relatedTarget;
    if (target instanceof Node && (area.value?.contains(target) || panel.value?.contains(target))) return;
    closePanel();
}
function closePanel() {
    ++panelPositionVersion;
    expanded.value = null; chooserPort.value = null; hoveredPort.value = null; hoveredPortRect = null;
}
function closeConnectionChooser() {
    const port = chooserPort.value;
    if (!port) return;
    ++panelPositionVersion;
    chooserPort.value = null;
    // The ordinary grouped-port panel does not require Shift.
    if (ports(port.direction).length <= 1) expanded.value = null;
    else expand(port.direction);
}
function endpoint(port: SemanticPort): GraphPortEndpoint {
    return { address: node.value.address!, direction: port.direction,
        kind: port.kind === "continuous" ? "sample" : "event", index: port.index, name: port.name };
}
function openConnectionChooser(port: SemanticPort, origin = hoveredPortRect) {
    if (!editable.value || dragPort.value || !editor?.scope.value
        || (chooserPort.value && portKey(chooserPort.value) === portKey(port))
        || connectionEndsAtPort(editor.scope.value, endpoint(port)).length < 1) return;
    chooserPort.value = port;
    expand(port.direction, origin);
}
function portIconRect(event: PointerEvent): DOMRect | null {
    const handle = event.currentTarget;
    if (!(handle instanceof HTMLElement)) return null;
    return (handle.querySelector<HTMLElement>(".port-circle") ?? handle).getBoundingClientRect();
}
function enterPort(event: PointerEvent, port: SemanticPort) {
    hoveredPort.value = port;
    hoveredPortRect = portIconRect(event);
    if (event.shiftKey) openConnectionChooser(port, hoveredPortRect);
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
    closePanel();
}
function accepts(port: SemanticPort) {
    if (!editable.value || !dragPort.value) return false;
    return dragEdit.value ? !!editor?.scope.value && canMoveConnectionEnd(editor.scope.value, dragEdit.value, endpoint(port))
        : acceptsPortConnection(dragPort.value, endpoint(port));
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
    else expanded.value = null;
});
watch(dragPort, (value) => { if (!value) expanded.value = null; });
watch(() => editor?.scope.value, closePanel);
watch([expanded, chooserPort], () => editor?.portsChanged(), { flush: "post" });
function viewportChanged(event: Event) {
    if (event.target instanceof Node && panel.value?.contains(event.target)) return;
    closePanel();
}
function dismissPanel(event: KeyboardEvent) {
    if (event.key === "Escape") closePanel();
    else if (!event.shiftKey) closeConnectionChooser();
    else if (event.key === "Shift" && hoveredPort.value) openConnectionChooser(hoveredPort.value);
}
function releasedModifier(event: KeyboardEvent) {
    if (!event.shiftKey) closeConnectionChooser();
}
function pointerModifier(event: PointerEvent) {
    // Pointer events carry modifier state even when keyboard focus is in a
    // different editor. No focus change is needed to recover a missed keyup.
    if (!event.shiftKey) closeConnectionChooser();
}
onMounted(() => {
    editor?.portsChanged();
    window.addEventListener("resize", viewportChanged);
    window.addEventListener("scroll", viewportChanged, true);
    window.addEventListener("keydown", dismissPanel);
    window.addEventListener("keyup", releasedModifier);
    window.addEventListener("blur", closePanel);
});
onBeforeUnmount(() => {
    editor?.portsChanged();
    window.removeEventListener("resize", viewportChanged);
    window.removeEventListener("scroll", viewportChanged, true);
    window.removeEventListener("keydown", dismissPanel);
    window.removeEventListener("keyup", releasedModifier);
    window.removeEventListener("blur", closePanel);
    if (dragPort.value && addressKey(dragPort.value.address) === nodeKey.value) editor?.cancelConnection();
});
</script>

<template>
    <div ref="area" class="node-ports" @mouseleave="leave" @pointermove="pointerModifier">
        <div v-for="direction in directions" :key="direction" :ref="(element) => setGroup(direction, element)"
            class="port-group" :data-port-group="direction" :data-node-key="nodeKey" :data-direction="direction"
            @mouseenter="hoverGroup(direction)">
            <div v-if="ports(direction).length > 1" class="port-slot" role="button" tabindex="0"
                :data-connection-state="groupConnections[direction].state" :class="{ pending: groupConnections[direction].pending }"
                :data-port-slot="node.address ? portSlotKey(node.address, direction) : undefined"
                :aria-label="`${ports(direction).length} ${direction} ports; ${groupConnections[direction].connected} connected`" :aria-expanded="expanded === direction"
                :title="`${ports(direction).length} ${direction} ports; ${groupConnections[direction].connected} connected`" @focus="expand(direction)" @keydown.enter="expand(direction)">
                <span class="port-circle stacked-first" /><span class="port-circle stacked-second" />
            </div>
            <button v-for="port in ports(direction).length <= 1 ? ports(direction) : []"
                :key="`${port.kind}:${port.index}`" type="button" class="port-handle"
                :class="{ connected: applied(port), pending: pending(port), target: accepts(port) }"
                :data-port-endpoint="JSON.stringify(endpoint(port))"
                :title="portTitle(port)"
                :aria-label="`${port.direction} ${port.name || port.index + 1}`"
                @pointerenter="enterPort($event, port)" @pointermove="enterPort($event, port)" @pointerleave="hoveredPort = null"
                @pointerdown="start($event, port)">
                <span class="port-circle" />
            </button>
        </div>
    </div>
    <Teleport to="body">
        <div v-if="expanded" ref="panel" class="port-panel" :class="{ above }" :style="panelPosition"
            :data-port-group="expanded" :data-node-key="nodeKey" :data-direction="expanded"
            @mouseleave="leave" @pointermove="pointerModifier" @keydown.esc="closePanel">
            <div class="expansion-body">
            <template v-if="chooserPort">
                <button v-for="end in choices" :key="end.connection.id" type="button" class="port-entry connection-choice"
                    :data-connection-end="JSON.stringify({ connectionId: end.connection.id, port: end.port })"
                    :class="{ connected: end.connection.applied, pending: !end.connection.applied }"
                    :title="`${chooserPort.name} · ${portDescription(chooserPort)}\n${connectionLabel(end.connection.connection)}`"
                    @pointerdown="pickConnection($event, end)">
                    <span class="port-circle" /><span class="port-name">{{ connectionLabel(end.connection.connection) }}</span>
                </button>
            </template>
            <template v-else>
            <button v-for="port in shownPorts" :key="`${port.direction}:${port.kind}:${port.index}`" type="button"
                class="port-entry" :class="{ target: accepts(port), connected: applied(port), pending: pending(port) }"
                :data-port-endpoint="JSON.stringify(endpoint(port))" :title="portTitle(port)"
                @pointerenter="enterPort($event, port)" @pointermove="enterPort($event, port)" @pointerleave="hoveredPort = null"
                @pointerdown="start($event, port)">
                <span class="port-circle" /><span class="port-name">{{ port.name || port.index + 1 }}</span>
                <small>{{ portDescription(port) }}</small>
            </button>
            </template>
            </div>
        </div>
    </Teleport>
</template>

<style scoped>
.node-ports { display: flex; align-items: center; justify-content: space-between; gap: 6px; margin-top: 5px; }
.port-group { display: flex; gap: 3px; min-height: 20px; }
[data-direction="input"] { --port-color: var(--vscode-terminal-ansiYellow); }
[data-direction="output"] { --port-color: var(--vscode-terminal-ansiBlue); }
.port-handle { display: grid; place-items: center; padding: 0; height: 22px; width: 22px; border: 0; background: transparent; cursor: crosshair; touch-action: none; }
.port-circle { display: inline-block; flex: 0 0 10px; width: 10px; height: 10px; box-sizing: border-box; border: 2px solid var(--port-color); border-radius: 50%; }
.connected .port-circle { background: var(--port-color); }
.pending .port-circle { border-style: dashed; }
.target .port-circle { box-shadow: 0 0 0 2px var(--vscode-editor-background), 0 0 0 3px var(--vscode-focusBorder); }
.port-slot { position: relative; width: 28px; height: 20px; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; }
.stacked-first { position: absolute; left: 5px; top: 4px; }
.stacked-second { position: absolute; left: 12px; top: 4px; background: var(--vscode-editor-background); }
.port-slot[data-connection-state="partial"] .stacked-second,
.port-slot[data-connection-state="full"] .port-circle { background: var(--port-color); }
.port-handle:focus-visible, .port-slot:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
.port-panel { position: fixed; z-index: 20; display: flex; flex-direction: column; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); box-shadow: 0 3px 12px #0005; }
.expansion-body { overflow-y: auto; min-height: 0; padding: 3px; }
.above .expansion-body { display: flex; flex-direction: column-reverse; }
.port-entry { display: flex; flex-shrink: 0; gap: 10px; align-items: center; width: 100%; padding: 5px; border: 0; background: transparent; color: inherit; text-align: left; cursor: crosshair; touch-action: none; }
.port-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.port-entry small { max-width: 45%; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-descriptionForeground); text-align: right; }
.port-entry:hover { background: var(--vscode-list-hoverBackground); }
</style>

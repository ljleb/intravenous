<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { addressKey, graphScopeNodeLabel, oppositePortDirection, scopeNodeForAddress, strongestConnectionFeedback,
    type ConnectionFeedback, type GraphScopeConnection } from "../../src/graphScope";
import { canMoveConnectionEnd, connectionEndsAtPort, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineNavigation } from "../timeline/timelineContext";
import { useNode } from "./nodeContext";
import { portDescription, portTypeSummary } from "./nodeSummary";
import type { SemanticNode, SemanticPort, SemanticPortDirection } from "./semanticNode";
import { acceptsPortConnection, canDropPortConnection } from "../../src/portConnectionGesture";
import { isTimelineSnapAddress } from "../../src/timelineSnap";
import { portEndpointKey, portSlotKey } from "../../src/selectedLaneConnections";
import ManualInput from "./ManualInput.vue";
import ImplicitOutputPort from "./ImplicitOutputPort.vue";
import NodePortRow from "./NodePortRow.vue";
import { useOptionalManualControls } from "./manualControlContext";
import { compactLanePortRowHeight, lanePortColumns, lanePortContinuationColumn, lanePortControlColumnWidth, lanePortControls, lanePortFoldedSocketHeight,
    lanePortLayout, lanePortRowGap, lanePortRowHeight } from "../../src/lanePortLayout";
import { usePresentationFrame } from "./presentationFrames";
import { usePresentationSize } from "./presentationSize";

const props = defineProps<{ inline: boolean; node?: SemanticNode; moduleBoundary?: boolean; shareLabelSpace?: boolean }>();
const presentedNode = useNode();
const node = computed(() => props.node ?? presentedNode.value);
const editor = useOptionalGraphEditor();
const timelineNavigation = useTimelineNavigation();
const manualControls = useOptionalManualControls();
const editingControl = computed(() => {
    void manualControls?.version.value;
    return !!node.value.address && !!manualControls?.gestures.isEditingNode(node.value.address);
});
const panel = ref<HTMLElement | null>(null);
const portArea = ref<HTMLElement | null>(null);
const portAreaSize = usePresentationSize(portArea);
const portAreaHeight = computed(() => portAreaSize.value?.height ?? compactLanePortRowHeight + lanePortFoldedSocketHeight);
const rowHeight = computed(() => lanePortRowHeight(portAreaHeight.value));
let header: HTMLElement | null = null;
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
const implicitOutput = computed(() => !!props.moduleBoundary && !!editor?.implicitOutputModules.value.has(nodeKey.value));
const implicitFeedback = computed(() => editor?.connectionFeedback.value.implicitOutputs.get(nodeKey.value));
const implicitPort = Symbol("implicit output");
const rowKey = ref<string>();
const directions: SemanticPortDirection[] = ["input", "output"];
const portDirection = (port: SemanticPort) => props.moduleBoundary ? oppositePortDirection(port.direction) : port.direction;
const ports = computed(() => ({
    input: node.value.ports.filter((port) => portDirection(port) === "input"),
    output: node.value.ports.filter((port) => portDirection(port) === "output"),
}));
const controls = computed(() => lanePortControls(portAreaSize.value?.width, rowHeight.value));
const socketsOnly = computed(() => !props.inline && !controls.value);
const hasKnob = (port: SemanticPort) => !props.moduleBoundary && !socketsOnly.value
    && port.kind === "continuous" && !!port.manualControl?.available;
const columnStyle = computed(() => ({
    "--port-row-height": `${rowHeight.value}px`,
    "--port-knob-size": `${controls.value?.knobSize ?? rowHeight.value}px`,
    "--port-row-gap": `${lanePortRowGap}px`,
    "--port-column-gap": `${lanePortColumns.gap}px`,
    "--port-aligned-control-width": editor ? `${lanePortControlColumnWidth(portAreaSize.value?.width)}px` : undefined,
    "--port-handle-width": `${controls.value?.handleWidth ?? lanePortColumns.handleWidth}px`,
    "--output-polarity-width": `${lanePortColumns.polarityWidth}px`,
    "--output-polarity-inset": `${lanePortColumns.polarityInset}px`,
}));
function layoutDirection(direction: SemanticPortDirection) {
    const available = props.inline ? 0 : portAreaHeight.value;
    const column = ports.value[direction];
    if (direction !== "output" || !implicitOutput.value) {
        const visible = lanePortLayout(column, available, rowHeight.value, lanePortFoldedSocketHeight);
        return { ...visible, implicitDirect: false, implicitFolded: false, foldedCount: visible.folded.length };
    }
    const { direct, folded, continuationOffset } = lanePortLayout<SemanticPort | typeof implicitPort>(
        [...column, implicitPort], available, rowHeight.value, lanePortFoldedSocketHeight);
    return {
        direct: direct.filter((port) => port !== implicitPort),
        folded: folded.filter((port) => port !== implicitPort),
        implicitDirect: direct.includes(implicitPort),
        implicitFolded: folded.includes(implicitPort),
        foldedCount: folded.length,
        continuationOffset,
    };
}
const layout = computed(() => ({ input: layoutDirection("input"), output: layoutDirection("output") }));
const shareLabels = computed(() => props.shareLabelSpace && !props.inline);
const portRows = computed(() => {
    const { input, output } = layout.value;
    const outputs: readonly (SemanticPort | typeof implicitPort)[] = output.implicitDirect ? [...output.direct, implicitPort] : output.direct;
    return Array.from({ length: Math.max(input.direct.length, outputs.length) }, (_, index) => ({
        input: input.direct[index], output: outputs[index],
    }));
});
const isDirect = (port: SemanticPort) => layout.value[portDirection(port)].direct.some((entry) => portKey(entry) === portKey(port));
const popoverHasKnob = (port: SemanticPort) => hasKnob(port) && !isDirect(port);
const editable = computed(() => !props.inline && !!editor?.scope.value?.editable && !!node.value.address);
const shiftHeld = computed(() => editor?.shiftHeld.value ?? false);
// Pointer coordinates change on every move; port controls only need the source.
const dragPort = computed(() => editor?.connectionDrag.value?.port ?? null);
const dragEdit = computed(() => editor?.connectionDrag.value?.edit);
const destinationDirection = computed(() => !dragPort.value ? null : dragEdit.value
    ? dragPort.value.direction : oppositePortDirection(dragPort.value.direction));
const choices = computed(() => chooserPort.value && editor?.scope.value
    ? connectionEndsAtPort(editor.scope.value, endpoint(chooserPort.value)) : []);
const portKey = (port: SemanticPort) => `${port.direction}:${port.kind}:${port.index}`;
const linksByPort = computed(() => {
    const links = new Map<string, GraphScopeConnection[]>();
    for (const entry of editor?.scope.value?.connections ?? []) {
        for (const port of node.value.ports) {
            if (entry.connection.kind !== (port.kind === "continuous" ? "sample" : "event")) continue;
            const matchers = portDirection(port) === "input" ? entry.connection.inputs : entry.connection.outputs;
            const matches = (matcher: typeof matchers[number]) => addressKey(matcher) === nodeKey.value
                && !!matcher.moduleBoundary === !!props.moduleBoundary
                && (matcher.port.name === undefined || matcher.port.name === port.name)
                && (matcher.port.index === undefined || matcher.port.index === port.index);
            if (!matchers.some(matches)) continue;
            const key = portKey(port);
            links.set(key, [...(links.get(key) ?? []), entry]);
        }
    }
    return links;
});
const portLinks = (port: SemanticPort) => linksByPort.value.get(portKey(port)) ?? [];
const applied = (port: SemanticPort) => portLinks(port).some((entry) => entry.applied);
const pending = (port: SemanticPort) => portLinks(port).length > 0 && !applied(port);
const portFeedback = (port: SemanticPort) => node.value.address
    ? editor?.connectionFeedback.value.ports.get(portEndpointKey(endpoint(port))) : undefined;
const groupConnections = computed(() => {
    const summarize = (direction: SemanticPortDirection) => {
        const { folded, foldedCount, implicitFolded } = layout.value[direction];
        const connected = folded.filter((port) => portLinks(port).length > 0).length + Number(implicitFolded);
        const kinds = [...new Set(folded.map((port) => port.kind))];
        const feedback = folded.reduce<ConnectionFeedback | undefined>((status, port) =>
            strongestConnectionFeedback(status, portFeedback(port)), implicitFolded ? implicitFeedback.value : undefined);
        return { state: connected === 0 ? "empty" : connected === foldedCount ? "full" : "partial",
            pending: folded.some(pending), connected, kinds, feedback };
    };
    return { input: summarize("input"), output: summarize("output") };
});
function setGroup(direction: SemanticPortDirection, element: unknown) {
    if (element instanceof HTMLElement) groups[direction] = element;
    else delete groups[direction];
}
function expandContinuation(direction: SemanticPortDirection) {
    if (props.inline || !layout.value[direction].foldedCount) return;
    const bounds = groups[direction]?.getBoundingClientRect();
    if (!bounds) return;
    const geometry = lanePortContinuationColumn(bounds, window.innerWidth, direction);
    const widened = geometry.left !== bounds.left || geometry.width > bounds.width;
    // A wider continuation can cross the other column. Start below any visible rows there.
    const visibleRowsEnd = widened ? Math.max(...directions.map((side) =>
        layout.value[side].continuationOffset - lanePortRowGap)) : 0;
    const rowTop = bounds.top + Math.max(layout.value[direction].continuationOffset, visibleRowsEnd);
    const footerBottom = header?.querySelector(".module-footer")?.getBoundingClientRect().bottom;
    const top = Math.max(rowTop, footerBottom ?? rowTop);
    ++panelPositionVersion;
    above.value = false;
    panelPosition.value = { left: `${geometry.left}px`, top: `${top}px`, width: `${geometry.width}px`,
        maxHeight: `${Math.max(0, window.innerHeight - top)}px` };
    panelPositioned.value = true;
    expanded.value = direction;
    editor?.portsChanged();
    void nextTick(checkPopoverPointer);
}
function expandConnectionChooser(direction: SemanticPortDirection, rect: DOMRect) {
    const version = ++panelPositionVersion;
    panelPositioned.value = false;
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
        const alignedLeft = Math.max(4, popup.left + dx);
        const availableWidth = chooserPort.value ? window.innerWidth - alignedLeft - 4 : window.innerWidth - 8;
        const width = Math.min(popup.width, Math.max(24, availableWidth));
        const left = chooserPort.value ? alignedLeft : Math.min(alignedLeft, Math.max(4, window.innerWidth - width - 4));
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
function headerPortDirection(target: EventTarget | null | undefined): SemanticPortDirection | undefined {
    if (!(target instanceof Element)) return;
    const control = target.closest("[data-port-control]");
    if (!control || !portArea.value?.contains(control)) return;
    const group = control.closest<HTMLElement>("[data-port-group]");
    const direction = group?.dataset.portGroup;
    return direction === "input" || direction === "output" ? direction : undefined;
}
function hoverPorts(target: EventTarget | null | undefined) {
    const direction = headerPortDirection(target);
    if (direction && !chooserPort.value && !dragPort.value && !editingControl.value && expanded.value !== direction)
        expandContinuation(direction);
}
function enterPortArea(event: PointerEvent) {
    hoverPorts(event.target);
    if (expanded.value && !editingControl.value) popoverPointer = { x: event.clientX, y: event.clientY };
}
const hoverContinuationFrame = usePresentationFrame("measure", () => {
    hoverPorts(editor?.elementAtPointer());
});
function scheduleHoveredContinuation() {
    if (props.inline || !editor || dragPort.value || editingControl.value || !header?.hasAttribute("data-hovered")) return;
    // Resizing can create a folded socket under a stationary pointer.
    hoverContinuationFrame.schedule();
}
function leave() {
    // Boundary events only request a check. A row replacement must not
    // overwrite the pointer position recorded by actual movement.
    checkPopoverPointer();
}
function crossingModuleFooter(target: Element | null, direction: SemanticPortDirection, point: { x: number; y: number }): boolean {
    if (!target || !header?.contains(target) || chooserPort.value) return false;
    const footer = header.querySelector(".module-footer")?.getBoundingClientRect();
    if (!footer || point.y < footer.top || point.y >= footer.bottom) return false;
    const column = groups[direction]?.querySelector(".port-column-hover")?.getBoundingClientRect();
    return !!column && point.x >= column.left && point.x <= column.right;
}
function checkPopoverPointer() {
    const direction = expanded.value;
    if (!direction || !panelPositioned.value || !popoverPointer || dragPort.value || editingControl.value) return;
    const current = document.elementFromPoint(popoverPointer.x, popoverPointer.y);
    if (panel.value?.contains(current) || headerPortDirection(current) === direction
        || crossingModuleFooter(current, direction, popoverPointer)) return;
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
    const target = editor?.elementAtPointer() ?? document.activeElement;
    const inPanel = !!target && !!panel.value?.contains(target);
    closePanel();
    if (inPanel && !isDirect(port)) expandContinuation(portDirection(port));
    else hoverPorts(target);
}
function endpoint(port: SemanticPort): GraphPortEndpoint {
    return { address: node.value.address!, direction: portDirection(port), moduleBoundary: props.moduleBoundary || undefined,
        kind: port.kind === "continuous" ? "sample" : "event", index: port.index, name: port.name };
}
function openConnectionChooser(port: SemanticPort, origin = hoveredPortRect) {
    if (!validOrigin(origin) || !editable.value || dragPort.value || !editor?.scope.value
        || (chooserPort.value && portKey(chooserPort.value) === portKey(port))
        || connectionEndsAtPort(editor.scope.value, endpoint(port)).length < 1) return;
    chooserPort.value = port;
    expandConnectionChooser(portDirection(port), origin);
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
    if (!editable.value || event.button !== 0) return;
    editor?.startConnection(event, end.port, end);
}
function accepts(port: SemanticPort) {
    if (!editable.value || !dragPort.value) return false;
    if ((node.value.address && isTimelineSnapAddress(node.value.address)) || isTimelineSnapAddress(dragPort.value.address)) {
        const drag = editor?.connectionDrag.value;
        return !!drag && canDropPortConnection(editor?.scope.value ?? null, drag, endpoint(port));
    }
    return dragEdit.value ? !!editor?.scope.value && canMoveConnectionEnd(editor.scope.value, dragEdit.value, endpoint(port))
        : acceptsPortConnection(dragPort.value, endpoint(port));
}
function groupAccepts(direction: SemanticPortDirection) {
    return layout.value[direction].folded.some(accepts);
}
function connectionLabel(connection: GraphScopeConnection["connection"]) {
    const label = (matcher: typeof connection.outputs[number]) => {
        const scope = editor?.scope.value;
        const node = scope && scopeNodeForAddress(scope, matcher);
        const name = node ? graphScopeNodeLabel(node) : "Unavailable node";
        return `${name}.${matcher.port.name || (matcher.port.index === undefined ? "*" : matcher.port.index + 1)}`;
    };
    return `${connection.outputs.map(label).join(", ")} → ${connection.inputs.map(label).join(", ")}`;
}
function portStatus(port: SemanticPort) {
    const links = portLinks(port);
    const pending = links.filter((entry) => !entry.applied).length;
    const first = links[0];
    const route = first?.sourceOwned && links.length === 1 ? "C++ connection"
        : first && links.length === 1 && first.connection.outputs.length === 1 && first.connection.inputs.length === 1
        ? connectionLabel(first.connection)
        : links.length ? `${links.length} ${links.length === 1 ? "connection" : "connections"}` : "";
    const name = `${port.displayName || port.name || port.index + 1} ${portDirection(port) === "input" ? "In" : "Out"}`;
    return [name, portFeedback(port) ? "Feedback loop" : "", route, pending ? `${pending} pending` : "",
        portTypeSummary(port)].filter(Boolean).join(" · ");
}
function rowProps(port: SemanticPort) {
    return { port, endpoint: endpoint(port), hasKnob: hasKnob(port), hint: portStatus(port), connected: applied(port),
        pending: pending(port), feedback: portFeedback(port), unavailable: destinationDirection.value === portDirection(port) && !accepts(port) };
}
watch(() => editor?.hoveredPortGroup.value, (group) => {
    if (!dragPort.value) return;
    if (group && group.nodeKey === nodeKey.value && group.rowKey === rowKey.value
        && group.direction === destinationDirection.value && groupAccepts(group.direction)) {
        expandContinuation(group.direction);
    }
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
watch(() => JSON.stringify(node.value.ports.map((port) => [port.direction, port.kind, port.index, port.name])), () => {
    closePanel();
    scheduleHoveredContinuation();
}, { flush: "post" });
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
watch([portAreaHeight, socketsOnly, () => portAreaSize.value?.width, () => props.inline,
    () => layout.value.input.continuationOffset, () => layout.value.output.continuationOffset,
    () => layout.value.input.foldedCount, () => layout.value.output.foldedCount, implicitOutput], () => {
    if (props.inline) closePanel();
    else if (expanded.value && !editingControl.value) {
        const direction = expanded.value;
        if (chooserPort.value) closePanel();
        else if (layout.value[direction].foldedCount && (!dragPort.value || groupAccepts(direction))) expandContinuation(direction);
        else closePanel();
    }
    editor?.portsChanged();
    scheduleHoveredContinuation();
}, { flush: "post" });
function viewportChanged(event: Event) {
    if (editingControl.value) return;
    if (event.target instanceof Node && panel.value?.contains(event.target)) return;
    closePanel();
}
function dismissPanel(event: KeyboardEvent) {
    if (event.key !== "Escape" || editingControl.value) return;
    hoverContinuationFrame.cancel();
    closePanel();
}
function windowBlurred() { if (!editingControl.value) closePanel(); }
function leaveFocus() {
    queueMicrotask(() => {
        const focused = document.activeElement;
        if (!expanded.value || editingControl.value || dragPort.value
            || headerPortDirection(focused) === expanded.value || panel.value?.contains(focused)) return;
        if (popoverPointer) checkPopoverPointer();
        else closePanel();
    });
}
onMounted(() => {
    header = portArea.value?.closest("header") ?? null;
    rowKey.value = header?.closest<HTMLElement>("[data-lane-row]")?.dataset.laneRow;
    header?.addEventListener("iv-hoverleave", leave);
    editor?.portsChanged();
    window.addEventListener("resize", viewportChanged);
    window.addEventListener("scroll", viewportChanged, true);
    window.addEventListener("keydown", dismissPanel);
    window.addEventListener("blur", windowBlurred);
});
onBeforeUnmount(() => {
    header?.removeEventListener("iv-hoverleave", leave);
    hoverContinuationFrame.cancel();
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
    <div ref="portArea" class="node-ports" :class="{ inline, 'sockets-only': socketsOnly, 'shared-labels': shareLabels }"
        :data-module-boundary="moduleBoundary ? '' : undefined"
        :style="[columnStyle, { '--folded-socket-height': `${lanePortFoldedSocketHeight}px` }]"
        @pointerover="enterPortArea" @focusin="hoverPorts($event.target)" @focusout="leaveFocus">
        <div v-if="shareLabels" class="paired-port-rows">
            <div v-for="(row, index) in portRows" :key="index" class="paired-port-row" :class="{ 'two-labels': row.input && row.output }">
                <template v-for="(port, direction) in row" :key="direction">
                    <ImplicitOutputPort v-if="typeof port === 'symbol'" :module-key="nodeKey" :feedback="implicitFeedback" />
                    <NodePortRow v-else-if="port" :key="portKey(port)" v-bind="rowProps(port)"
                        :data-port-group="direction" :data-node-key="nodeKey"
                        :data-folded-ports="layout[direction].foldedCount ? direction : undefined"
                        @handle-hover="enterPort($event, port)" @handle-leave="hoveredPort = null"
                        @wire-start="start($event, port)" />
                </template>
            </div>
        </div>
        <div v-for="direction in directions" :key="direction" :ref="(element) => setGroup(direction, element)"
            class="port-group" :data-port-group="direction" :data-node-key="nodeKey" :data-direction="direction"
            :data-folded-ports="!inline && layout[direction].foldedCount ? direction : undefined">
            <span v-if="!inline && layout[direction].foldedCount" class="port-column-hover" data-port-control aria-hidden="true" />
            <template v-if="!shareLabels">
                <NodePortRow v-for="port in layout[direction].direct" :key="portKey(port)" v-bind="rowProps(port)"
                    @handle-hover="enterPort($event, port)" @handle-leave="hoveredPort = null" @wire-start="start($event, port)" />
                <ImplicitOutputPort v-if="layout[direction].implicitDirect" :module-key="nodeKey" :feedback="implicitFeedback" />
            </template>
            <component :is="inline ? 'span' : 'button'" v-if="layout[direction].foldedCount" class="port-slot"
                :type="inline ? undefined : 'button'" :role="inline ? 'img' : undefined"
                :class="{ cropped: !inline, single: layout[direction].foldedCount === 1,
                    'implicit-only': layout[direction].implicitFolded && !layout[direction].folded.length,
                    pending: groupConnections[direction].pending, unavailable: !inline && destinationDirection === direction && !groupAccepts(direction) }"
                :data-connection-state="groupConnections[direction].state"
                :data-connection-feedback="groupConnections[direction].feedback"
                :data-port-slot="node.address ? portSlotKey(node.address, direction, moduleBoundary) : undefined"
                :data-implicit-output="layout[direction].implicitFolded ? nodeKey : undefined"
                :aria-label="`${layout[direction].foldedCount}${inline ? '' : ' more'} ${direction} ports, ${groupConnections[direction].connected} connected`"
                :aria-expanded="inline ? undefined : expanded === direction && !chooserPort"
                :aria-disabled="inline ? undefined : destinationDirection === direction && !groupAccepts(direction)"
                :data-status-hint="`${layout[direction].foldedCount}${inline ? '' : ' more'} ${direction} ports · ${groupConnections[direction].connected} connected${groupConnections[direction].feedback ? ' · Feedback loop' : ''}`"
                data-port-control v-on="inline ? {} : { click: (event: MouseEvent) => hoverPorts(event.target) }">
                <span class="port-circle stacked-first"
                    :data-port-kind="inline || groupConnections[direction].kinds.length === 1 ? groupConnections[direction].kinds[0] : undefined" />
                <span v-if="inline && layout[direction].foldedCount > 1" class="port-circle stacked-second"
                    :data-port-kind="groupConnections[direction].kinds.at(-1)" />
            </component>
        </div>
    </div>
    <Teleport to="body">
        <div v-if="expanded && !inline" ref="panel" class="port-panel"
            :class="{ above, continuation: !chooserPort, positioning: !panelPositioned, 'sockets-only': socketsOnly }"
            :style="[panelPosition, columnStyle]"
            :aria-hidden="!panelPositioned"
            :data-lane-key="moduleBoundary ? undefined : nodeKey"
            :data-lane-row="rowKey" :data-module-boundary="moduleBoundary ? '' : undefined"
            :data-port-group="expanded" :data-node-key="nodeKey"
            @iv-hoverleave="leave" @focusout="leaveFocus" @keydown.esc="closePanel"
            @wheel="timelineNavigation?.wheel($event)" @pointerdown.capture="timelineNavigation?.beginMiddleDrag($event)">
            <div v-if="chooserPort" class="expansion-body" tabindex="-1">
                <div v-for="(end, index) in choices" :key="end.connection.id" class="port-row"
                    :data-direction="end.port.direction"
                    :class="{ 'with-knob': index === 0 && popoverHasKnob(chooserPort) }">
                    <button type="button" tabindex="-1" class="port-entry connection-choice"
                        :data-port-kind="end.port.kind"
                        :data-connection-feedback="end.connection.feedback"
                        :data-connection-end="JSON.stringify({ connectionId: end.connection.id, port: end.port })"
                        :class="{ connected: end.connection.applied, pending: !end.connection.applied }"
                        :data-status-hint="`${end.connection.feedback ? 'Feedback loop · ' : end.connection.applied ? '' : 'Pending · '}${connectionLabel(end.connection.connection)}`"
                        :aria-description="`${chooserPort.name} · ${portDescription(chooserPort)}`"
                        @pointerdown="pickConnection($event, end)">
                        <span class="port-circle" /><span class="port-name">{{ connectionLabel(end.connection.connection) }}</span>
                    </button>
                    <ManualInput v-if="index === 0 && popoverHasKnob(chooserPort)" :name="chooserPort.name" :direction="chooserPort.direction" />
                </div>
            </div>
            <div v-else class="expansion-body" tabindex="-1" :data-direction="expanded">
                <NodePortRow v-for="port in layout[expanded].folded" :key="portKey(port)" v-bind="rowProps(port)"
                    @handle-hover="enterPort($event, port)" @handle-leave="hoveredPort = null" @wire-start="start($event, port)" />
                <ImplicitOutputPort v-if="layout[expanded].implicitFolded" :module-key="nodeKey" :feedback="implicitFeedback" />
            </div>
        </div>
    </Teleport>
</template>

<style scoped>
.node-ports, .port-panel {
    --port-toggle-width: calc(var(--output-polarity-width) - var(--output-polarity-inset));
    --port-control-column: var(--port-aligned-control-width, calc(var(--port-knob-size) + var(--port-toggle-width)));
    --port-knob-column-size: calc(var(--port-control-column) - var(--port-toggle-width));
    --port-knob-edge-gap: calc((var(--port-knob-column-size) - var(--port-knob-size)) / 2);
    --knob-socket-tuck: 10px;
    --port-socket-size: 12px;
    --port-knob-center-tuck: calc((var(--port-knob-column-size) + var(--port-socket-size)) / 2);
}
:is(.node-ports, .port-panel).sockets-only { --port-control-column: 0px; --port-knob-center-tuck: 0px; }
.node-ports { display: grid;
    grid-template-columns: minmax(min-content, 1fr) fit-content(calc((100% - var(--port-column-gap)) / 2));
    grid-template-rows: minmax(0, 1fr); gap: var(--port-column-gap); margin-top: 5px; min-height: 22px; }
.node-ports.inline {
    grid-template-columns: repeat(2, minmax(0, 1fr)); gap: var(--inline-port-gap);
}
.node-ports.inline > .port-group[data-direction="output"] { align-items: flex-end; }
.node-ports.shared-labels { grid-template-columns: repeat(2, minmax(0, 1fr)); }
.shared-labels > .port-group { grid-row: 1; }
.shared-labels > .port-group[data-direction="input"] { grid-column: 1; }
.shared-labels > .port-group[data-direction="output"] { grid-column: 2; }
.paired-port-rows {
    grid-area: 1 / 1 / 2 / -1; position: relative; z-index: 1; pointer-events: none;
    display: flex; flex-direction: column; gap: var(--port-row-gap);
}
.paired-port-row {
    --port-label-gap: 0px;
    /* Each label grows equally until it reaches its intrinsic width. Spare
       space between two complete names goes to the flexible middle track. */
    display: grid; grid-template-rows: var(--port-row-height);
    grid-template-columns: var(--port-control-column) var(--port-handle-width) minmax(0, max-content)
        minmax(var(--port-label-gap), 1fr) minmax(0, max-content) var(--port-handle-width) var(--port-control-column);
}
.paired-port-row.two-labels { --port-label-gap: var(--port-column-gap); }
.paired-port-row > :deep(.control-row) { grid-row: 1; grid-template-columns: subgrid; pointer-events: auto; }
.paired-port-row > :deep(.control-row[data-direction="input"]) { grid-column: 1 / 4; }
.paired-port-row > :deep(.control-row[data-direction="output"]) { grid-column: 5 / -1; }
.port-group { position: relative; isolation: isolate; display: flex; flex-direction: column; gap: var(--port-row-gap); min-height: 0; }
.port-column-hover { position: absolute; z-index: -1; inset-block: 0; inset-inline-start: 0;
    width: min(100%, calc(var(--port-control-column) + var(--port-handle-width))); }
[data-direction="output"] > .port-column-hover { inset-inline-start: auto; inset-inline-end: 0; }
.node-ports:not(.inline) .port-group[data-folded-ports] { min-width: calc(var(--port-handle-width) + var(--port-control-column)); }
[data-direction="input"] { --port-base-color: var(--iv-port-input); --port-color: var(--port-base-color); }
[data-direction="output"] { --port-base-color: var(--iv-port-output); --port-color: var(--port-base-color); }
.port-circle {
    --port-circle-muted: color-mix(in srgb, var(--port-color) var(--iv-disconnected-port-strength), var(--vscode-editor-background));
    --port-circle-fill: var(--port-circle-muted);
    display: inline-block; flex: 0 0 12px; width: 12px; height: 12px; box-sizing: border-box;
    border-radius: 50%; background: var(--port-circle-fill);
}
[data-port-kind="event"] .port-circle, .port-circle[data-port-kind="event"] { border-radius: 25%; }
.pending .port-circle { border: 2px dashed var(--port-circle-outline, var(--port-color)); }
.port-slot { position: relative; flex: 0 0 var(--port-slot-height, 22px); width: min(28px, 100%); height: var(--port-slot-height, 22px);
    box-sizing: border-box; padding: 0; border: 1px solid transparent; border-radius: 7px; background: transparent; }
.port-slot.implicit-only { --port-color: var(--vscode-descriptionForeground); }
button.port-slot { cursor: pointer; }
.inline .port-slot { width: var(--inline-socket-group-width); }
.inline .port-slot .port-circle { width: var(--inline-symbol-size); height: auto; aspect-ratio: 1; }
.stacked-first, .stacked-second { position: absolute; top: 50%; transform: translate(-50%, -50%); }
.inline .stacked-first { left: calc(50% - var(--inline-stack-spacing) / 2); }
.inline .stacked-second { left: calc(50% + var(--inline-stack-spacing) / 2); }
.inline .single .stacked-first { left: 50%; }
.port-slot.cropped {
    --socket-center-inset: calc(var(--port-control-column) - var(--port-knob-center-tuck) + var(--port-socket-size) / 2);
    position: absolute; bottom: calc(-1 * var(--module-footer-height, 0px)); left: calc(var(--socket-center-inset) - var(--port-handle-width) / 2);
    width: var(--port-handle-width); height: var(--folded-socket-height);
    overflow: hidden; border: 0; border-radius: 4px 4px 0 0; }
.port-group[data-direction="output"] > .cropped { left: auto; right: calc(var(--socket-center-inset) - var(--port-handle-width) / 2); }
.cropped .port-circle { top: calc(var(--folded-socket-height) - var(--port-socket-size) / 2); left: 50%;
    width: var(--port-socket-size); transform: translateX(-50%); }
.connected .port-circle,
.cropped[data-connection-state="partial"] .port-circle,
.port-slot[data-connection-state="partial"] .stacked-second,
.port-slot[data-connection-state="full"] .port-circle {
    --port-circle-fill: var(--port-color);
    --port-circle-outline: var(--port-circle-muted);
}
button.port-slot[data-hovered] { background: color-mix(in srgb, var(--port-color) 12%, transparent); }
.port-panel { position: fixed; z-index: 20; display: flex; flex-direction: column; box-sizing: border-box; border: 1px solid var(--vscode-panel-border); border-radius: 7px; background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); box-shadow: 0 3px 12px #0005; }
.port-panel.continuation { border: 0; background: transparent; box-shadow: none; }
.port-panel.positioning { visibility: hidden; pointer-events: none; }
.expansion-body { display: flex; flex-direction: column; gap: var(--port-row-gap); overflow-y: auto; overflow-x: hidden; min-height: 0; padding: 3px; }
.continuation .expansion-body { padding: 0 0 3px; border-radius: 5px;
    background: var(--vscode-editorWidget-background); pointer-events: auto;
    box-shadow: inset 0 0 0 1px var(--vscode-editorWidget-border, var(--vscode-panel-border)), 0 3px 10px #0005; }
/* Keep the output columns at the right edge when the list scrolls. */
.continuation .expansion-body[data-direction="output"] { direction: rtl; }
.continuation .expansion-body[data-direction="output"] > .control-row { direction: ltr; }
.above .expansion-body { display: flex; flex-direction: column-reverse; }
.port-row { display: flex; flex-shrink: 0; align-items: center; min-width: 0; }
.port-row .port-entry { flex: 1; min-width: 0; width: auto; }
.port-row.with-knob { --chooser-columns: 24px calc(38px + var(--output-polarity-width) - var(--output-polarity-inset)) minmax(0, 1fr);
    display: grid; grid-template-columns: var(--chooser-columns); }
.port-row.with-knob[data-direction="input"] { --chooser-columns: calc(37px + var(--output-polarity-width)) 24px minmax(0, 1fr); }
.with-knob > .port-entry { grid-column: 1 / -1; grid-row: 1; display: grid;
    grid-template-columns: var(--chooser-columns); gap: 0; width: 100%; box-sizing: border-box; padding-inline: 0 5px; }
.with-knob > .port-entry .port-circle { grid-column: 1; }
.with-knob[data-direction="input"] > .port-entry .port-circle { grid-column: 2; }
.with-knob > .port-entry .port-name { grid-column: 3; }
.with-knob > .manual-input { grid-column: 2; grid-row: 1; z-index: 1; }
.with-knob[data-direction="input"] > .manual-input { grid-column: 1; }
.with-knob[data-direction="input"] > .manual-input { gap: 0; }
.port-entry { display: flex; flex-shrink: 0; gap: 10px; align-items: center; width: 100%; padding: 5px; border: 0; background: transparent; color: inherit; text-align: left; cursor: crosshair; touch-action: none; }
.port-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--iv-port-label-base); }
.port-entry[data-hovered] { background: var(--vscode-list-hoverBackground); }
.unavailable { --port-color: color-mix(in srgb, var(--port-base-color) 25%, var(--vscode-disabledForeground) 75%); color: var(--vscode-disabledForeground); }
[data-connection-feedback] .port-circle { --port-circle-fill: var(--vscode-errorForeground); border: 0; }
[data-connection-feedback="edited"] .port-circle { outline: 1px solid var(--vscode-errorForeground); outline-offset: 2px; }
</style>

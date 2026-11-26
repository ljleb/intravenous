<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { addressKey, graphScopeNodeLabel, matchingScopePorts, oppositePortDirection, scopeNodeForAddress, strongestConnectionFeedback,
    type ConnectionFeedback, type GraphScopeConnection } from "../../src/graphScope";
import { canDetachConnection, canMoveConnectionEnd, connectionEndsAtPort, type GraphConnectionEnd, type GraphPortEndpoint } from "../../src/graphConnectionEditing";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineNavigation } from "../timeline/timelineContext";
import { useNode } from "./nodeContext";
import { portDescription, portDisplayName, portTypeSummary } from "./nodeSummary";
import type { SemanticNode, SemanticPort, SemanticPortDirection } from "./semanticNode";
import { acceptsPortConnection, canDropPortConnection } from "../../src/portConnectionGesture";
import { isGeneratedSnapConnection, isTimelineSnapAddress } from "../../src/timelineSnap";
import { portEndpointKey, portSlotKey } from "../../src/selectedLaneConnections";
import ManualInput from "./ManualInput.vue";
import ConnectionDelayKnob from "./ConnectionDelayKnob.vue";
import ImplicitOutputPort from "./ImplicitOutputPort.vue";
import NodePortRow from "./NodePortRow.vue";
import PortSocketControl from "./PortSocketControl.vue";
import { useOptionalManualControls } from "./manualControlContext";
import { compactLanePortRowHeight, lanePortColumns, lanePortContinuationColumn, lanePortControlColumnWidth, lanePortControls, lanePortFoldedSocketHeight,
    lanePortLayout, lanePortRowGap, lanePortRowHeight } from "../../src/lanePortLayout";
import { usePresentationSize } from "./presentationSize";
import { usePresentationFrame } from "./presentationFrames";

const props = defineProps<{ inline: boolean; node?: SemanticNode; moduleBoundary?: boolean; shareLabelSpace?: boolean }>();
const presentedNode = useNode();
const node = computed(() => props.node ?? presentedNode.value);
const editor = useOptionalGraphEditor();
const timelineNavigation = useTimelineNavigation();
const manualControls = useOptionalManualControls();
const editingControl = computed(() => {
    void manualControls?.version.value;
    const knob = editor?.activeKnob.value;
    return (!!node.value.address && !!manualControls?.gestures.isEditingNode(node.value.address))
        || (!!knob && (choices.value.some((end) => delayKnobIdentity(end) === knob)
            || foldedRows.value.some(({ delay }) => delay?.identity === knob)));
});
const portList = ref<HTMLElement | null>(null);
const chooserPanel = ref<HTMLElement | null>(null);
const portArea = ref<HTMLElement | null>(null);
const portAreaSize = usePresentationSize(portArea);
const portAreaHeight = computed(() => portAreaSize.value?.height ?? compactLanePortRowHeight + lanePortFoldedSocketHeight);
const rowHeight = computed(() => lanePortRowHeight(portAreaHeight.value));
let header: HTMLElement | null = null;
const expanded = ref<SemanticPortDirection | null>(null);
const chooserPort = ref<SemanticPort | null>(null);
const chooserPortName = computed(() => {
    const port = chooserPort.value;
    return port ? portDisplayName(port) : "";
});
type ConnectionChooserMode = "move" | "detach";
const chooserMode = ref<ConnectionChooserMode>("move");
const popoverOpen = computed(() => expanded.value !== null || chooserPort.value !== null);
let popoverPointer: { x: number; y: number } | null = null;
let dismissedChooserBounds: DOMRect | undefined;
let chooserPositionVersion = 0;
const chooserPositioned = ref(false);
const chooserPosition = ref<Record<string, string>>({});
const listPosition = ref<Record<string, string>>({});
const above = ref(false);
const groups: Partial<Record<SemanticPortDirection, HTMLElement>> = {};
const nodeKey = computed(() => node.value.address ? addressKey(node.value.address) : "");
const implicitOutput = computed(() => !!props.moduleBoundary && !!editor?.implicitOutputModules.value.has(nodeKey.value));
const implicitFeedback = computed(() => editor?.connectionFeedback.value.implicitOutputs.get(nodeKey.value));
const implicitPort = Symbol("implicit output");
const rowKey = ref<string>();
const panelAttributes = computed(() => ({
    "data-lane-key": props.moduleBoundary ? undefined : nodeKey.value,
    "data-lane-row": rowKey.value,
    "data-module-boundary": props.moduleBoundary ? "" : undefined,
    "data-node-key": nodeKey.value,
}));
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
// Pointer coordinates change on every move; port controls only need the source.
const dragPort = computed(() => editor?.connectionDrag.value?.port ?? null);
const dragEdit = computed(() => editor?.connectionDrag.value?.edit);
const destinationDirection = computed(() => !dragPort.value ? null : dragEdit.value
    ? dragPort.value.direction : oppositePortDirection(dragPort.value.direction));
const choices = computed(() => chooserPort.value && editor?.scope.value
    ? portConnectionChoices(chooserPort.value, chooserMode.value) : []);
const chooserRows = computed(() => choices.value.map((end, index) => ({
    end,
    label: chooserMode.value === "detach"
        ? `${end.connection.connection.loopExtraLatency === undefined ? 'Detach' : 'Attach'} ${connectionLabel(end.connection.connection)}`
        : connectionLabel(end.connection.connection),
    delay: end.connection.connection.loopExtraLatency !== undefined,
    manual: index === 0 && !!chooserPort.value && popoverHasKnob(chooserPort.value),
})));
const foldedRows = computed(() => !expanded.value ? [] : layout.value[expanded.value].folded.map((port) => {
    const links = portLinks(port);
    const connection = links[0];
    const end = editable.value && !dragPort.value && links.length === 1 && !connection.sourceOwned
        && connection.connection.loopExtraLatency !== undefined ? { connection, port: endpoint(port) } : undefined;
    return { port, delay: end ? { end, identity: delayKnobIdentity(end) } : undefined };
}));
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
    listPosition.value = { left: `${geometry.left}px`, top: `${top}px`, width: `${geometry.width}px`,
        maxHeight: `${Math.max(0, window.innerHeight - top)}px` };
    expanded.value = direction;
    editor?.portsChanged();
    void nextTick(checkPopoverPointer);
}
function positionConnectionChooser(rect: DOMRect) {
    const version = ++chooserPositionVersion;
    chooserPositioned.value = false;
    above.value = window.innerHeight - rect.top < 180;
    chooserPosition.value = {
        left: `${Math.max(4, Math.min(rect.left, window.innerWidth - 32))}px`,
        width: "max-content", maxWidth: `${window.innerWidth - 8}px`,
        ...(above.value ? { bottom: `${Math.max(4, window.innerHeight - rect.bottom)}px` }
            : { top: `${Math.max(4, rect.top)}px` }),
        maxHeight: `${Math.max(40, Math.min(360, above.value ? rect.bottom - 4 : window.innerHeight - rect.top - 4))}px`,
    };
    alignFirstRow(rect, version);
}
function alignFirstRow(origin: DOMRect, version: number) {
    // Measure the rendered row, rather than assuming a font size or line height.
    void nextTick(() => {
        if (version !== chooserPositionVersion || !chooserPanel.value || !chooserPort.value) return;
        const firstCircle = chooserPanel.value.querySelector<HTMLElement>("[data-connection-end] .port-circle");
        if (!firstCircle) return;
        const circle = firstCircle.getBoundingClientRect();
        const popup = chooserPanel.value.getBoundingClientRect();
        if (!firstCircle.isConnected || !validOrigin(circle) || !validOrigin(popup)
            || version !== chooserPositionVersion) return;
        const dx = origin.left + origin.width / 2 - circle.left - circle.width / 2;
        const dy = origin.top + origin.height / 2 - circle.top - circle.height / 2;
        const alignedLeft = Math.max(4, popup.left + dx);
        const availableWidth = window.innerWidth - alignedLeft - 4;
        const width = Math.min(popup.width, Math.max(24, availableWidth));
        if (above.value) {
            const bottom = window.innerHeight - popup.bottom - dy;
            chooserPosition.value = { left: `${alignedLeft}px`, bottom: `${bottom}px`, width: `${width}px`,
                maxHeight: `${Math.max(24, Math.min(360, window.innerHeight - bottom - 4))}px` };
        } else {
            const top = popup.top + dy;
            chooserPosition.value = { left: `${alignedLeft}px`, top: `${top}px`, width: `${width}px`,
                maxHeight: `${Math.max(24, Math.min(360, window.innerHeight - top - 4))}px` };
        }
        chooserPositioned.value = true;
        editor?.portsChanged();
        // The pointer may have left while layout events were being deferred.
        // Wait for the positioned style to reach the DOM before hit-testing.
        void nextTick(() => {
            if (version === chooserPositionVersion) checkPopoverPointer();
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
    if (popoverOpen.value && !editingControl.value) popoverPointer = { x: event.clientX, y: event.clientY };
}
const hoverContinuationFrame = usePresentationFrame("measure", () => hoverPorts(editor?.elementAtPointer()));
function scheduleHoveredContinuation() {
    if (!props.inline && !dragPort.value && !editingControl.value && header?.hasAttribute("data-hovered"))
        hoverContinuationFrame.schedule();
}
function openFoldedPorts(event: MouseEvent, direction: SemanticPortDirection) {
    if (dragPort.value || editingControl.value) return;
    event.preventDefault(); event.stopPropagation();
    closeConnectionChooser();
    if (expanded.value !== direction) expandContinuation(direction);
    if (expanded.value) popoverPointer = { x: event.clientX, y: event.clientY };
}
function insidePopover(target: Node | null) {
    return !!target && (portList.value?.contains(target) || chooserPanel.value?.contains(target));
}
function leave() {
    // Boundary events only request a check. A row replacement must not
    // overwrite the pointer position recorded by actual movement.
    checkPopoverPointer();
}
function checkPopoverPointer() {
    const direction = expanded.value ?? (chooserPort.value && portDirection(chooserPort.value));
    if (!direction || !popoverPointer || dragPort.value || editingControl.value
        || (chooserPort.value && (!chooserPositioned.value || (!expanded.value && chooserMode.value === "move")))) return;
    const { x, y } = popoverPointer;
    const current = document.elementFromPoint(x, y);
    if (insidePopover(current) || headerPortDirection(current) === direction) {
        dismissedChooserBounds = undefined;
        return;
    }
    const bounds = dismissedChooserBounds;
    if (bounds && x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) return;
    closePanel();
    hoverPorts(current);
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
    closeConnectionChooser();
    if (portList.value) editor?.closeKnobValueWithin(portList.value);
    expanded.value = null;
}
function closeConnectionChooser(keepHover = false) {
    // A chooser can extend beyond the list. Treat its former bounds as part
    // of the return path until the pointer reaches the list or moves away.
    dismissedChooserBounds = keepHover && expanded.value ? chooserPanel.value?.getBoundingClientRect() : undefined;
    if (!chooserPort.value) return;
    if (chooserPanel.value) editor?.closeKnobValueWithin(chooserPanel.value);
    ++chooserPositionVersion;
    chooserPositioned.value = false;
    chooserPort.value = null;
}
function dismissConnectionContext(event: MouseEvent) {
    if (event.target instanceof Element && event.target.closest(".knob-control, .manual-input, .manual-value-popover")) return;
    event.preventDefault(); event.stopPropagation();
    if (editingControl.value) return;
    popoverPointer = { x: event.clientX, y: event.clientY };
    closeConnectionChooser(true);
}
function endpoint(port: SemanticPort): GraphPortEndpoint {
    return { address: node.value.address!, direction: portDirection(port), moduleBoundary: props.moduleBoundary || undefined,
        kind: port.kind === "continuous" ? "sample" : "event", index: port.index, name: port.name };
}
function delayKnobIdentity(end: GraphConnectionEnd): string {
    return `feedback-delay:${end.connection.id}:${portEndpointKey(end.port)}`;
}
function portConnectionChoices(port: SemanticPort, mode: ConnectionChooserMode) {
    const scope = editor?.scope.value;
    if (!scope || !node.value.address) return [];
    const ends = connectionEndsAtPort(scope, endpoint(port));
    if (mode === "detach") return ends.filter((end) => canDetachConnection(end.connection.connection));
    return ends;
}
function openConnectionChooser(port: SemanticPort, origin: DOMRect | null, mode: ConnectionChooserMode = "move") {
    if (!validOrigin(origin) || !editable.value || dragPort.value || !editor?.scope.value
        || !portConnectionChoices(port, mode).length) return false;
    dismissedChooserBounds = undefined;
    chooserPort.value = port;
    chooserMode.value = mode;
    positionConnectionChooser(origin);
    return true;
}
function portIconRect(event: Event): DOMRect | null {
    const control = event.currentTarget;
    if (!(control instanceof HTMLElement) || !control.isConnected || control.closest(".port-panel.positioning")) return null;
    const circle = control.closest(".control-row")?.querySelector(".port-handle .port-circle");
    const rect = (circle ?? control).getBoundingClientRect();
    return validOrigin(rect) ? rect : null;
}
function openPortConnections(event: MouseEvent, port: SemanticPort) {
    if (!openConnectionChooser(port, portIconRect(event))) return;
    popoverPointer = { x: event.clientX, y: event.clientY };
}
function start(event: PointerEvent, port: SemanticPort) {
    if (!editable.value || event.button !== 0 || !editor?.scope.value) return;
    const origin = portIconRect(event);
    const altClick = () => void nextTick(() => {
        if (!editable.value) return;
        const ends = portConnectionChoices(port, "detach");
        if (ends.length === 1) editor.toggleConnectionDetach(ends[0]);
        else if (ends.length > 1) openConnectionChooser(port, origin, "detach");
    });
    if (!event.shiftKey) { editor.startConnection(event, endpoint(port), undefined, altClick); return; }
    event.preventDefault(); event.stopPropagation();
    const ends = portConnectionChoices(port, "move");
    if (ends.length === 1) editor.startConnection(event, endpoint(port), ends[0], altClick);
}
function pickConnection(event: PointerEvent, end: GraphConnectionEnd) {
    if (!editable.value || event.button !== 0 || chooserMode.value === "detach") return;
    const altClick = canDetachConnection(end.connection.connection) ? () => editor?.toggleConnectionDetach(end) : undefined;
    editor?.startConnection(event, end.port, end, altClick);
}
function pickDetach(end: GraphConnectionEnd) {
    if (!editable.value || chooserMode.value !== "detach") return;
    editor?.toggleConnectionDetach(end);
    closeConnectionChooser(true);
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
    const label = (matcher: typeof connection.outputs[number], direction: SemanticPortDirection) => {
        const scope = editor?.scope.value;
        const node = scope && scopeNodeForAddress(scope, matcher);
        const name = node ? graphScopeNodeLabel(node) : "Unavailable node";
        if (matcher.port.name === undefined && matcher.port.index === undefined) return `${name}.*`;
        const ports = node ? matchingScopePorts(node, matcher, connection.kind, direction) : [];
        const port = ports.length === 1 ? portDisplayName(ports[0].port)
            : matcher.port.name || String((matcher.port.index ?? 0) + 1);
        return `${name}.${port}`;
    };
    return `${connection.outputs.map((matcher) => label(matcher, "output")).join(", ")} → ${connection.inputs.map((matcher) => label(matcher, "input")).join(", ")}`;
}
function portStatus(port: SemanticPort) {
    const links = portLinks(port);
    const pending = links.filter((entry) => !entry.applied).length;
    const first = links[0];
    const route = first?.sourceOwned && links.length === 1 ? "C++ connection"
        : first && links.length === 1 && first.connection.outputs.length === 1 && first.connection.inputs.length === 1
        ? connectionLabel(first.connection)
        : links.length ? `${links.length} ${links.length === 1 ? "connection" : "connections"}` : "";
    const name = portDisplayName(port);
    const direction = portDirection(port) === "input" ? "In" : "Out";
    return [name, name.toLowerCase() === direction.toLowerCase() ? "" : direction,
        portFeedback(port) ? "Feedback loop" : "", route,
        first?.connection.loopExtraLatency !== undefined && links.length === 1 ? `Detached · ${first.connection.loopExtraLatency} smp` : "",
        pending ? `${pending} pending` : "",
        portTypeSummary(port)].filter(Boolean).join(" · ");
}
function rowProps(port: SemanticPort) {
    return { port, endpoint: endpoint(port), hasKnob: hasKnob(port), hint: portStatus(port), connected: applied(port),
        connectionChooser: editable.value && portLinks(port).some((entry) => !entry.sourceOwned && !isGeneratedSnapConnection(entry)),
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
watch(popoverOpen, (open) => {
    if (open) {
        window.addEventListener("pointermove", movePopoverPointer, true);
        window.addEventListener("pointerout", leavePopoverWindow, true);
    } else {
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
    else {
        if (chooserPort.value && !choices.value.length) closeConnectionChooser();
        void nextTick(checkPopoverPointer);
    }
});
watch(choices, (entries) => { if (chooserPort.value && !entries.length && !editingControl.value) closeConnectionChooser(); });
watch([expanded, chooserPort], () => editor?.portsChanged(), { flush: "post" });
watch([portAreaHeight, socketsOnly, () => portAreaSize.value?.width, () => props.inline,
    () => layout.value.input.continuationOffset, () => layout.value.output.continuationOffset,
    () => layout.value.input.foldedCount, () => layout.value.output.foldedCount, implicitOutput], () => {
    if (props.inline) closePanel();
    else if (!editingControl.value) {
        const direction = expanded.value;
        if (direction && layout.value[direction].foldedCount && (!dragPort.value || groupAccepts(direction))) {
            closeConnectionChooser();
            expandContinuation(direction);
        } else closePanel();
    }
    editor?.portsChanged();
    scheduleHoveredContinuation();
}, { flush: "post" });
function viewportChanged(event: Event) {
    if (editingControl.value) return;
    if (event.target instanceof Node && insidePopover(event.target)) return;
    closePanel();
}
function dismissPanel(event: KeyboardEvent) {
    if (event.key !== "Escape" || !popoverOpen.value || editingControl.value) return;
    if (chooserPort.value) closeConnectionChooser(true);
    else closePanel();
}
function windowBlurred() { if (!editingControl.value) closePanel(); }
function dismissOutsideChooser(event: PointerEvent) {
    const target = event.target;
    if (!(target instanceof Element) || chooserPanel.value?.contains(target) || target.closest(".manual-value-popover")) return;
    if (portList.value?.contains(target)) {
        if (event.button !== 2) closeConnectionChooser();
    } else closePanel();
}
watch(chooserPort, (port, _previous, onCleanup) => {
    if (!port) return;
    window.addEventListener("pointerdown", dismissOutsideChooser, true);
    onCleanup(() => window.removeEventListener("pointerdown", dismissOutsideChooser, true));
});
function leaveFocus() {
    queueMicrotask(() => {
        const focused = document.activeElement;
        if (!expanded.value || popoverPointer || chooserPort.value || editingControl.value || dragPort.value
            || headerPortDirection(focused) === expanded.value || insidePopover(focused)) return;
        closePanel();
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
                        @wire-start="start($event, port)" @handle-context="openPortConnections($event, port)" />
                </template>
            </div>
        </div>
        <div v-for="direction in directions" :key="direction" :ref="(element) => setGroup(direction, element)"
            class="port-group" :data-port-group="direction" :data-node-key="nodeKey" :data-direction="direction"
            :data-folded-ports="!inline && layout[direction].foldedCount ? direction : undefined">
            <span v-if="!inline && layout[direction].foldedCount" class="port-column-hover" data-port-control aria-hidden="true" />
            <template v-if="!shareLabels">
                <NodePortRow v-for="port in layout[direction].direct" :key="portKey(port)" v-bind="rowProps(port)"
                    @wire-start="start($event, port)" @handle-context="openPortConnections($event, port)" />
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
                :aria-expanded="inline ? undefined : expanded === direction"
                :aria-disabled="inline ? undefined : destinationDirection === direction && !groupAccepts(direction)"
                :data-status-hint="`${layout[direction].foldedCount}${inline ? '' : ' more'} ${direction} ports · ${groupConnections[direction].connected} connected${groupConnections[direction].feedback ? ' · Feedback loop' : ''}`"
                data-port-control @click="!inline && openFoldedPorts($event, direction)"
                @contextmenu.stop.prevent="!inline && openFoldedPorts($event, direction)">
                <span class="port-circle stacked-first"
                    :data-port-kind="inline || groupConnections[direction].kinds.length === 1 ? groupConnections[direction].kinds[0] : undefined" />
                <span v-if="inline && layout[direction].foldedCount > 1" class="port-circle stacked-second"
                    :data-port-kind="groupConnections[direction].kinds.at(-1)" />
            </component>
        </div>
    </div>
    <Teleport to="body">
        <div v-if="expanded && !inline" ref="portList" class="port-panel continuation" :class="{ 'sockets-only': socketsOnly }"
            :style="[listPosition, columnStyle]" role="dialog" :aria-label="expanded === 'input' ? 'Input ports' : 'Output ports'"
            v-bind="panelAttributes" :data-port-group="expanded"
            @iv-hoverleave="leave" @focusout="leaveFocus"
            @wheel="timelineNavigation?.wheel($event)" @pointerdown.capture="timelineNavigation?.beginMiddleDrag($event)">
            <div class="expansion-body" tabindex="-1" :data-direction="expanded">
                <NodePortRow v-for="{ port, delay } in foldedRows" :key="portKey(port)" v-bind="rowProps(port)" :delay="delay"
                    @wire-start="start($event, port)" @handle-context="openPortConnections($event, port)" />
                <ImplicitOutputPort v-if="layout[expanded].implicitFolded" :module-key="nodeKey" :feedback="implicitFeedback" />
            </div>
        </div>
        <div v-if="chooserPort && !inline" ref="chooserPanel" class="port-panel connection-chooser"
            :class="{ above, positioning: !chooserPositioned, 'sockets-only': socketsOnly }"
            :style="[chooserPosition, columnStyle]" role="dialog" :aria-hidden="!chooserPositioned"
            :aria-label="`Connections for ${chooserPortName}`"
            v-bind="panelAttributes" :data-port-group="portDirection(chooserPort)"
            @iv-hoverleave="leave" @contextmenu.capture="dismissConnectionContext"
            @wheel="timelineNavigation?.wheel($event)" @pointerdown.capture="timelineNavigation?.beginMiddleDrag($event)">
            <button v-if="expanded" type="button" class="back-to-ports" data-port-popover-navigation aria-label="Back to ports"
                @click="closeConnectionChooser(true)">‹ Ports</button>
            <div class="expansion-body" tabindex="-1">
                <PortSocketControl v-for="{ end, label, delay, manual } in chooserRows" :key="end.connection.id"
                    :endpoint="end.port" :connection-id="end.connection.id" :connected="end.connection.applied"
                    :pending="!end.connection.applied" :feedback="end.connection.feedback" :label="label"
                    :description="`${chooserPort.name} · ${portDescription(chooserPort)}`"
                    :hint="`${end.connection.feedback ? 'Feedback loop · ' : end.connection.applied ? '' : 'Pending · '}${label}`"
                    @wire-start="pickConnection($event, end)" @pick="pickDetach(end)">
                    <template v-if="delay || manual" #knob>
                        <ConnectionDelayKnob v-if="delay" class="connection-delay-knob" :end="end" :identity="delayKnobIdentity(end)"
                            :port-name="chooserPortName" port-cutout />
                        <ManualInput v-else :name="chooserPort.name" :direction="chooserPort.direction" port-cutout />
                    </template>
                    <template v-if="delay && manual" #secondary>
                        <ManualInput :name="chooserPort.name" :direction="chooserPort.direction" />
                    </template>
                </PortSocketControl>
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
    position: absolute; bottom: 0; left: calc(var(--socket-center-inset) - var(--port-handle-width) / 2);
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
.port-panel.connection-chooser { z-index: 21; }
.port-panel.positioning { visibility: hidden; pointer-events: none; }
.expansion-body { display: flex; flex-direction: column; gap: var(--port-row-gap); overflow-y: auto; overflow-x: hidden; min-height: 0; padding: 3px; }
.continuation .expansion-body { padding: 0 0 3px; border-radius: 5px;
    background: var(--vscode-editorWidget-background); pointer-events: auto;
    box-shadow: inset 0 0 0 1px var(--vscode-editorWidget-border, var(--vscode-panel-border)), 0 3px 10px #0005; }
/* Keep the output columns at the right edge when the list scrolls. */
.continuation .expansion-body[data-direction="output"] { direction: rtl; }
.continuation .expansion-body[data-direction="output"] > .control-row { direction: ltr; }
.above .expansion-body { display: flex; flex-direction: column-reverse; }
.back-to-ports { align-self: start; flex-shrink: 0; margin: 3px; padding: 2px 5px; border: 0;
    border-radius: 4px; background: transparent; color: var(--vscode-foreground); cursor: pointer; }
.back-to-ports:hover { background: var(--vscode-list-hoverBackground); }
.unavailable { --port-color: color-mix(in srgb, var(--port-base-color) 25%, var(--vscode-disabledForeground) 75%); color: var(--vscode-disabledForeground); }
[data-connection-feedback] .port-circle { --port-circle-fill: var(--vscode-errorForeground); border: 0; }
[data-connection-feedback="edited"] .port-circle { outline: 1px solid var(--vscode-errorForeground); outline-offset: 2px; }
</style>

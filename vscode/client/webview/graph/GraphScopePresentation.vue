<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from "vue";
import { addressKey, nodeWidth, nodeHeightFor, resolveScopePorts, zoomAt, type GraphPortMatcher, type GraphScopeNode, type GraphScopePort, type GraphScopeConnection, type Point } from "../../src/graphScope";
import { useNode } from "../presentation/nodeContext";
import PresentationHost from "../presentation/PresentationHost.vue";
import { semanticNodeFromScopeChild } from "../presentation/semanticNode";
import { useGraphEditor } from "./context";
import type { NodeViewChoice } from "../presentation/displayMode";

const node = useNode();
const scope = computed(() => node.value.scope!);
const children = computed(() => scope.value.nodes.map((child) => ({
    child, semantic: semanticNodeFromScopeChild(child, scope.value),
})));
const { layout, save } = useGraphEditor();
const canvas = ref<HTMLElement | null>(null);
const viewport = computed(() => layout.value.viewport);
const position = (child: GraphScopeNode) => layout.value.positions[addressKey(child.address)] ?? { x: 0, y: 0 };
const inputs = (child: GraphScopeNode) => [...child.sampleInputs.map((port) => ({ ...port, kind: 'sample' })), ...child.eventInputs.map((port) => ({ ...port, kind: 'event' }))];
const outputs = (child: GraphScopeNode) => [...child.sampleOutputs.map((port) => ({ ...port, kind: 'sample' })), ...child.eventOutputs.map((port) => ({ ...port, kind: 'event' }))];
const height = nodeHeightFor;
const choice = computed<NodeViewChoice>(() => viewport.value.scale < 0.5 ? "nano" : viewport.value.scale < 0.8 ? "mini" : "editable");
const portTitle = (port: GraphScopePort) => `${port.name}: ${port.eventType ?? (port.valueTypes ?? []).map((value) => value.type).join(' | ')}${port.channelType ? ` (${port.channelType})` : ''}`;

function endpoints(matcher: GraphPortMatcher, kind: "sample" | "event", direction: "input" | "output"): Point[] {
    return resolveScopePorts(scope.value, matcher, kind, direction).map((found) => {
        const origin = position(found.node);
        return { x: origin.x + (direction === "output" ? nodeWidth : 0), y: origin.y + 64 + found.row * 24 };
    });
}
const wires = computed(() => scope.value.connections.flatMap((entry) => {
    const result: Array<{ key: string; path: string; applied: boolean; title: string }> = [];
    entry.connection.outputs.forEach((output, sourceIndex) => {
        entry.connection.inputs.forEach((input, targetIndex) => {
            const sources = endpoints(output, entry.connection.kind, "output");
            const targets = endpoints(input, entry.connection.kind, "input");
            sources.forEach((source, sourcePort) => targets.forEach((target, targetPort) => {
                const bend = Math.max(60, Math.abs(target.x - source.x) / 2);
                result.push({ key: `${entry.id}:${sourceIndex}:${targetIndex}:${sourcePort}:${targetPort}`, applied: entry.applied, title: entry.id,
                    path: `M ${source.x} ${source.y} C ${source.x + bend} ${source.y}, ${target.x - bend} ${target.y}, ${target.x} ${target.y}` });
            }));
        });
    });
    return result;
}));
function unresolved(entry: GraphScopeConnection) {
    return entry.connection.outputs.some((matcher) => !endpoints(matcher, entry.connection.kind, "output").length)
        || entry.connection.inputs.some((matcher) => !endpoints(matcher, entry.connection.kind, "input").length);
}
function matcherLabel(matcher: GraphPortMatcher) {
    return `${matcher.instanceId}${matcher.path?.length ? ` / ${JSON.stringify(matcher.path)}` : ''} : ${matcher.port.name ?? matcher.port.index ?? 'default'}${matcher.port.channel !== undefined ? ` [${matcher.port.channel}]` : ''}`;
}

let drag: { pointer: number; start: Point; initial: Point; nodeKey?: string } | null = null;
function down(event: PointerEvent) {
    if (event.button !== 0 && event.button !== 1) return;
    const target = event.target as Element;
    const handle = target.closest<HTMLElement>("[data-node-drag]");
    if (!handle && target.closest(".node-card, .connection-list")) return;
    const key = event.button === 0 ? handle?.dataset.nodeDrag : undefined;
    drag = { pointer: event.pointerId, start: { x: event.clientX, y: event.clientY },
        initial: { ...(key ? layout.value.positions[key] : viewport.value) }, nodeKey: key };
    canvas.value?.setPointerCapture(event.pointerId);
    event.preventDefault();
}
function move(event: PointerEvent) {
    if (!drag || drag.pointer !== event.pointerId) return;
    const divisor = drag.nodeKey ? viewport.value.scale : 1;
    const next = { x: drag.initial.x + (event.clientX - drag.start.x) / divisor,
        y: drag.initial.y + (event.clientY - drag.start.y) / divisor };
    if (drag.nodeKey) layout.value.positions[drag.nodeKey] = next;
    else Object.assign(layout.value.viewport, next);
}
function up(event: PointerEvent) {
    if (!drag || drag.pointer !== event.pointerId) return;
    drag = null;
    if (canvas.value?.hasPointerCapture(event.pointerId)) canvas.value.releasePointerCapture(event.pointerId);
    save();
}
let wheelSave: ReturnType<typeof setTimeout> | undefined;
function wheel(event: WheelEvent) {
    if ((event.target as Element).closest(".connection-list")) return;
    event.preventDefault();
    const rect = canvas.value!.getBoundingClientRect();
    const pixels = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? rect.height : 1);
    layout.value.viewport = zoomAt(viewport.value, { x: event.clientX - rect.left, y: event.clientY - rect.top },
        viewport.value.scale * Math.exp(-pixels * 0.001));
    clearTimeout(wheelSave);
    wheelSave = setTimeout(() => { wheelSave = undefined; save(); }, 150);
}
function fit() {
    if (!scope.value.nodes.length || !canvas.value) return;
    const nodes = scope.value.nodes;
    const left = Math.min(...nodes.map((child) => position(child).x));
    const top = Math.min(...nodes.map((child) => position(child).y));
    const right = Math.max(...nodes.map((child) => position(child).x + nodeWidth));
    const bottom = Math.max(...nodes.map((child) => position(child).y + height(child)));
    const rect = canvas.value.getBoundingClientRect();
    const scale = Math.max(0.1, Math.min(1, (rect.width - 80) / (right - left), (rect.height - 80) / (bottom - top)));
    layout.value.viewport = { x: 40 - left * scale, y: 40 - top * scale, scale };
    save();
}
onBeforeUnmount(() => {
    if (wheelSave) clearTimeout(wheelSave);
    if (wheelSave || drag) save();
});
</script>

<template>
    <section class="scope-presentation">
        <header class="graph-toolbar">
            <strong>Project graph</strong>
            <span>{{ scope.nodes.length }} nodes · {{ scope.connections.length }} connections</span>
            <button type="button" @click="fit">Fit graph</button>
            <span>{{ Math.round(viewport.scale * 100) }}%</span>
            <span class="hint">Drag a title to move · drag the background to pan · scroll to zoom</span>
        </header>
        <div ref="canvas" class="canvas" tabindex="0" aria-label="Project graph"
            @pointerdown="down" @pointermove="move" @pointerup="up" @pointercancel="up" @lostpointercapture="up" @wheel="wheel">
            <div class="world" :style="{ transform: `translate(${viewport.x}px, ${viewport.y}px) scale(${viewport.scale})` }">
                <svg class="wires" aria-hidden="true">
                    <path v-for="wire in wires" :key="wire.key" :d="wire.path" :class="{ pending: !wire.applied }"><title>{{ wire.title }}</title></path>
                </svg>
                <article v-for="{ child, semantic } in children" :key="addressKey(child.address)" class="node-card"
                    :class="{ unrealized: !child.instance.realized }"
                    :style="{ left: `${position(child).x}px`, top: `${position(child).y}px`, width: `${nodeWidth}px`, height: `${height(child)}px` }">
                    <header :data-node-drag="addressKey(child.address)" class="node-title" :title="child.instance.instanceId">
                        {{ child.instance.displayName || child.instance.definitionId }}
                        <span v-if="!child.instance.realized">(not realized)</span>
                    </header>
                    <div v-for="(port, row) in inputs(child)" :key="`in:${port.kind}:${port.index}`" class="socket input"
                        :class="port.kind" :style="{ top: `${64 + row * 24}px` }" :title="`Input ${portTitle(port)}`" />
                    <div v-for="(port, row) in outputs(child)" :key="`out:${port.kind}:${port.index}`" class="socket output"
                        :class="port.kind" :style="{ top: `${64 + row * 24}px` }" :title="`Output ${portTitle(port)}`" />
                    <div class="node-content">
                        <PresentationHost :node="semantic" :view-choice="choice" :scale="viewport.scale" />
                    </div>
                </article>
            </div>
            <p v-if="!scope.nodes.length" class="empty">No project instances. Use Intravenous: Open Modules to create an instance.</p>
            <details v-if="scope.connections.length" class="connection-list">
                <summary>Connections ({{ scope.connections.length }})</summary>
                <div v-for="entry in scope.connections" :key="entry.id" class="connection-record">
                    <strong>{{ entry.id }}</strong> · {{ entry.connection.kind }} · {{ entry.applied ? 'applied' : 'not applied' }}
                    <div v-for="(matcher, index) in entry.connection.outputs" :key="`out:${index}`">From {{ matcherLabel(matcher) }}</div>
                    <div v-for="(matcher, index) in entry.connection.inputs" :key="`in:${index}`">To {{ matcherLabel(matcher) }}</div>
                    <div v-if="unresolved(entry)">An endpoint is missing from this scope or selects a module interior.</div>
                </div>
            </details>
        </div>
    </section>
</template>

<style scoped>
.scope-presentation { height: 100%; display: flex; flex-direction: column; }
.graph-toolbar { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; padding: 8px 12px; border-bottom: 1px solid var(--vscode-panel-border); }
.graph-toolbar button { color: var(--vscode-button-foreground); background: var(--vscode-button-background); border: 0; padding: 4px 8px; cursor: pointer; }
.hint { opacity: .7; }
.canvas { position: relative; flex: 1; min-height: 200px; overflow: hidden; touch-action: none; background-image: radial-gradient(var(--vscode-editorWhitespace-foreground, #7775) 1px, transparent 1px); background-size: 24px 24px; }
.world { position: absolute; transform-origin: 0 0; }
.wires { position: absolute; width: 1px; height: 1px; overflow: visible; pointer-events: none; }
.wires path { fill: none; stroke: var(--vscode-charts-blue, #559ddd); stroke-width: 2; }
.wires .pending { stroke-dasharray: 6 4; stroke: var(--vscode-editorWarning-foreground); }
.node-card { position: absolute; box-sizing: border-box; border: 1px solid var(--vscode-panel-border, #777); border-radius: 6px; background: var(--vscode-editorWidget-background); box-shadow: 0 3px 10px #0003; }
.node-card.unrealized { border-style: dashed; }
.node-title { height: 36px; box-sizing: border-box; padding: 8px 12px; background: var(--vscode-sideBar-background); border-radius: 6px 6px 0 0; cursor: grab; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.node-content { position: absolute; inset: 40px 12px 8px; overflow: auto; }
.socket { position: absolute; width: 10px; height: 10px; border-radius: 50%; background: var(--vscode-charts-blue, #559ddd); transform: translateY(-50%); }
.socket.input { left: -6px; }.socket.output { right: -6px; }.socket.event { background: var(--vscode-charts-orange, #d98c38); }
.connection-list { position: absolute; right: 12px; bottom: 12px; max-width: 420px; max-height: 40%; overflow: auto; padding: 8px; background: var(--vscode-editorWidget-background); border: 1px solid var(--vscode-panel-border); overflow-wrap: anywhere; }
.connection-record { padding: 8px 0; border-bottom: 1px solid var(--vscode-panel-border); }
.empty { padding: 20px; }
</style>

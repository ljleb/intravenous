<script setup lang="ts">
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import { computed, nextTick, ref } from "vue";
import { nodeDisplayName, portDescription } from "./nodeSummary";
import { editableHeightPx, type NodeDisplayMode } from "./displayMode";
import NodePorts from "./NodePorts.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";
import { addressKey, formatLaneLabel } from "../../src/graphScope";
import { useOptionalGraphEditor } from "../graph/context";

const props = defineProps<{ mode?: NodeDisplayMode; flushContent?: boolean; moduleBoundary?: boolean }>();

const node = useNode();
const lane = useLane();
const editor = useOptionalGraphEditor();
const canExpand = computed(() => !props.moduleBoundary && !!editor && node.value.hasChildren
    && node.value.loadState === "ready" && !!node.value.address);
const expanded = computed(() => !!node.value.address && (editor?.expandedModules.value.has(addressKey(node.value.address)) ?? false));
const disclosureHint = computed(() => {
    if (!expanded.value) return "Expand module";
    const address = node.value.address;
    const children = address && editor?.scope.value?.childScopes?.find(({ subject }) => addressKey(subject) === addressKey(address));
    return children?.nodes.length === 0 && !children.hasBoundary ? "No addressable children · Collapse module" : "Collapse module";
});
const editableHeight = `${editableHeightPx}px`;
const inlineHeader = computed(() => props.mode === "mini" || props.mode === "nano"
    || (props.mode === "compact" && (lane?.regionHeight.value ?? Infinity) < editableHeightPx));
const inputs = computed(() => node.value.ports.filter((port) => port.direction === "input"));
const outputs = computed(() => node.value.ports.filter((port) => port.direction === "output"));
const inputSummary = computed(() => inputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const outputSummary = computed(() => outputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const name = computed(() => props.moduleBoundary ? "Module Ports" : nodeDisplayName(node.value));
const sourceLabel = computed(() => props.moduleBoundary ? undefined : node.value.sourceLabel);
const virtualMembers = computed(() => !props.moduleBoundary && node.value.virtualNode ? node.value.memberCount : undefined);
const memberDescription = computed(() => {
    const count = virtualMembers.value;
    return count === undefined ? "" : `Virtual lane · ${count} ${count === 1 ? 'member' : 'members'}`;
});
const laneStatus = computed(() => props.moduleBoundary ? `Public ports of ${nodeDisplayName(node.value)}`
    : node.value.loadState === "waitingForDefinition" ? "Waiting for the node definition"
    : node.value.loadState === "waitingForGraph" ? "Waiting for graph realization"
    : node.value.errorMessage || (sourceLabel.value ? formatLaneLabel(name.value, sourceLabel.value) : node.value.definitionId));
const nameInput = ref<HTMLInputElement | null>(null);
const nameEdit = ref<{ original: string; draft: string } | null>(null);

async function beginRename() {
    if (!lane?.rename || nameEdit.value) return;
    nameEdit.value = { original: node.value.displayName ?? "", draft: name.value };
    await nextTick();
    nameInput.value?.focus({ preventScroll: true });
    nameInput.value?.select();
}

function finishRename(commit: boolean) {
    const edit = nameEdit.value;
    if (!edit) return;
    nameEdit.value = null;
    if (!commit || edit.draft === edit.original) return;
    const nextName = edit.draft.trim();
    if (nextName !== edit.original) lane?.rename?.(nextName, edit.original);
}
</script>

<template>
    <div class="node-lane" :class="{ 'inline-header': inlineHeader }" :data-mode="mode">
        <header class="lane-label">
            <div class="lane-heading">
                <svg class="lane-identity" viewBox="0 0 16 16" aria-hidden="true">
                    <path v-if="moduleBoundary" d="M6 2v12m4-12v12M2 5h4m4 6h4" />
                    <path v-else-if="virtualMembers !== undefined" d="m2 6 6-3 6 3-6 3zm0 3 6 3 6-3M2 12l6 3 6-3" />
                    <path v-else :d="lane?.icon?.value ?? 'M4 3h8l2 2v6l-2 2H4l-2-2V5z'" />
                </svg>
                <input v-if="nameEdit" ref="nameInput" v-model="nameEdit.draft" class="lane-name lane-name-edit"
                    aria-label="Lane name" :placeholder="node.defaultDisplayName" spellcheck="false" tabindex="-1"
                    @blur="finishRename(true)" @keydown.enter="!$event.isComposing && finishRename(true)"
                    @keydown.esc.stop.prevent="finishRename(false)" />
                <span v-else class="lane-title" :class="{ 'has-source-label': sourceLabel }"
                    :data-status-hint="laneStatus" :data-status-shortcuts="lane?.rename ? 'Double-click to rename' : undefined"
                    :aria-description="laneStatus" @dblclick="beginRename">
                    <span class="lane-name" :class="{ failed: node.loadState === 'failed' }">{{ name }}</span>
                    <span v-if="sourceLabel" class="lane-source-label">{{ sourceLabel }}</span>
                </span>
                <span v-if="virtualMembers !== undefined" class="member-count" :aria-label="memberDescription"
                    :data-status-hint="memberDescription">{{ virtualMembers }}</span>
                <button v-if="lane && !moduleBoundary" type="button" tabindex="-1" class="copy-debug" data-status-hint="Copy node debug details"
                    aria-label="Copy node debug details" @click="lane.copyDebugDetails()">⧉</button>
            </div>
            <button v-if="canExpand" type="button" tabindex="-1" class="module-disclosure"
                :aria-expanded="expanded" :aria-label="expanded ? 'Collapse children' : 'Expand children'"
                :data-status-hint="disclosureHint" @click.stop="editor?.toggleModule(node.address!)">
                <svg viewBox="0 0 12 12" aria-hidden="true"><path :d="expanded ? 'm3 4.5 3 3 3-3' : 'm4.5 3 3 3-3 3'" /></svg>
            </button>
            <NodePorts v-if="node.address" :inline="inlineHeader" :module-boundary="moduleBoundary" />
            <div v-else class="port-summary">
                <span :aria-label="`${inputs.length} inputs`" :aria-description="inputSummary">→ {{ inputs.length }}</span>
                <span :aria-label="`${outputs.length} outputs`" :aria-description="outputSummary">{{ outputs.length }} →</span>
            </div>
        </header>
        <div v-if="!moduleBoundary" class="lane-content" :class="{ 'flush-content': flushContent || !$slots.default }">
            <slot><LiveOutputMeters :mode="mode" /></slot>
        </div>
    </div>
</template>

<style scoped>
.node-lane { display: grid; grid-template-columns: var(--node-label-width, 220px) minmax(0, 1fr); grid-template-rows: minmax(0, 1fr); height: 100%; min-height: 0; container: node-lane / size; }
.lane-label {
    /* At the editable boundary, leave 32px for a compact knob and the folded socket. */
    --header-growth: clamp(0px, calc(100cqh - v-bind(editableHeight)), 25px);
    --heading-height: calc(14px + var(--header-growth) * .24);
    position: relative; display: flex; flex-direction: column; min-width: 0; min-height: 0;
    padding: calc(1px + var(--header-growth) * .28) 12px 0 17px; border-right: 1px solid var(--vscode-panel-border);
}
.lane-heading { display: flex; flex: 0 0 var(--heading-height); align-items: flex-start; gap: 6px;
    padding-inline-start: var(--iv-lane-indent, 0px); }
.lane-identity, .module-disclosure svg { fill: none; stroke: currentColor; stroke-width: 1.25;
    stroke-linecap: round; stroke-linejoin: round; }
.lane-identity { flex: 0 0 auto; width: min(13px, 100cqh); height: min(13px, 100cqh); color: var(--vscode-icon-foreground); }
.member-count { flex: 0 0 auto; color: var(--vscode-descriptionForeground); font-size: min(11px, calc(100cqh - 3px));
    font-variant-numeric: tabular-nums; line-height: 1.1; }
.module-disclosure { position: absolute; z-index: 1; inset-inline-start: 1px; inset-block-end: 1px; display: grid;
    place-items: center; width: 15px; height: min(16px, calc(100cqh - 1px)); border: 0; border-radius: 3px;
    padding: 0; color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; }
.module-disclosure svg { width: min(12px, 100cqh); height: min(12px, 100cqh); }
.module-disclosure[data-hovered] { background: var(--vscode-toolbar-hoverBackground); }
.lane-label :deep(.node-ports) { flex: 1; min-height: 32px; margin-top: calc(var(--header-growth) * .2); }
.port-summary { display: flex; gap: 16px; margin-top: 6px; font-size: .9em; color: var(--vscode-descriptionForeground); }
.inline-header .lane-label { flex-direction: row; align-items: center; gap: 4px; padding: 0 4px 0 17px; }
.inline-header .lane-heading { flex: 1; min-width: 0; overflow: hidden; align-items: center; }
.inline-header .lane-label :deep(.node-ports) {
    --port-slot-height: min(22px, 100cqh);
    margin-top: 0; flex: 0 1 60px; min-width: 0; min-height: 0; height: var(--port-slot-height);
}
.inline-header .port-summary { display: none; }
.lane-title { flex: 1; display: flex; align-items: baseline; gap: 6px; min-width: 0; }
.lane-name, .lane-source-label { min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    line-height: 1; }
.lane-name {
    font-size: min(calc(100cqh - 4px), var(--heading-height), var(--vscode-font-size, 13px));
    color: var(--iv-lane-title-color); font-weight: 500; }
.has-source-label .lane-name { max-width: 68%; }
.lane-source-label { font-size: min(calc(100cqh - 4px), var(--heading-height), calc(var(--vscode-font-size, 13px) * .85));
    color: var(--vscode-descriptionForeground); }
.lane-name.failed { color: var(--vscode-errorForeground, #f14c4c); }
.lane-name-edit { flex: 1; box-sizing: border-box; width: 0; padding: 0; border: 0; outline: none;
    background: var(--vscode-input-background); color: var(--vscode-input-foreground); }
.copy-debug { flex: 0 0 20px; height: var(--heading-height); padding: 0; border: 0; border-radius: 3px;
    color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; font-size: min(16px, var(--heading-height)); line-height: 1; }
.inline-header .copy-debug { height: min(100cqh, var(--heading-height)); font-size: min(100cqh, 14px, var(--heading-height)); }
.copy-debug[data-hovered] { background: var(--vscode-toolbar-hoverBackground); }
.lane-content { min-width: 0; min-height: 0; overflow: hidden; padding: clamp(0px, calc(12.5cqh - 2px), 4px) 0; box-sizing: border-box;
    content-visibility: auto; }
.lane-content.flush-content { padding: 0; }
</style>

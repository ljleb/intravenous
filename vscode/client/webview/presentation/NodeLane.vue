<script setup lang="ts">
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import { computed, nextTick, ref } from "vue";
import { nodeDisplayName, portDescription } from "./nodeSummary";
import { editableHeightPx, type NodeDisplayMode } from "./displayMode";
import NodePorts from "./NodePorts.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";
import { addressKey, formatLaneLabel, isProjectInstance, laneLabels } from "../../src/graphScope";
import { useOptionalGraphEditor } from "../graph/context";

const props = defineProps<{ mode?: NodeDisplayMode; flushContent?: boolean; moduleBoundary?: boolean }>();

const node = useNode();
const lane = useLane();
const editor = useOptionalGraphEditor();
const moduleChild = computed(() => !props.moduleBoundary && !!node.value.address && !isProjectInstance(node.value.address));
const canExpand = computed(() => !props.moduleBoundary && !!editor && node.value.hasChildren
    && node.value.loadState === "ready" && !!node.value.address);
const expanded = computed(() => !!node.value.address && (editor?.expandedModules.value.has(addressKey(node.value.address)) ?? false));
const footerAction = computed(() => expanded.value ? "Collapse module" : "Expand module");
const editableHeight = `${editableHeightPx}px`;
const inlineHeader = computed(() => props.mode === "mini" || props.mode === "nano"
    || (props.mode === "compact" && (lane?.regionHeight.value ?? Infinity) < editableHeightPx));
const inputs = computed(() => node.value.ports.filter((port) => port.direction === "input"));
const outputs = computed(() => node.value.ports.filter((port) => port.direction === "output"));
const inputSummary = computed(() => inputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const outputSummary = computed(() => outputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const displayName = computed(() => nodeDisplayName(node.value));
const labels = computed(() => props.moduleBoundary ? laneLabels("Module Ports")
    : laneLabels(displayName.value, node.value.sourceLabel));
const symbolicMembers = computed(() => !props.moduleBoundary && node.value.symbolicNode ? node.value.memberCount : undefined);
const memberDescription = computed(() => {
    const count = symbolicMembers.value;
    return count === undefined ? "" : `Symbolic lane · ${count} ${count === 1 ? 'member' : 'members'}`;
});
const laneStatus = computed(() => props.moduleBoundary ? `Inner boundary of ${displayName.value}`
    : node.value.loadState === "waitingForDefinition" ? "Waiting for the node definition"
    : node.value.loadState === "waitingForGraph" ? "Waiting for graph realization"
    : node.value.errorMessage || [
        labels.value.secondary ? formatLaneLabel(labels.value) : node.value.definitionId,
        moduleChild.value ? "C++ module child" : "",
    ].filter(Boolean).join(" · "));
const nameInput = ref<HTMLInputElement | null>(null);
const nameEdit = ref<{ original: string; draft: string } | null>(null);

async function beginRename() {
    if (!lane?.rename || nameEdit.value) return;
    nameEdit.value = { original: node.value.displayName ?? "", draft: displayName.value };
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
    <div class="node-lane" :class="{ 'inline-header': inlineHeader, 'module-child': moduleChild,
        'boundary-lane': moduleBoundary }" :data-mode="mode">
        <header class="lane-label iv-graph-header" :class="{ 'has-module-footer': canExpand }">
            <div class="lane-header-main">
                <div class="lane-heading" :data-status-hint="formatLaneLabel(labels)">
                    <svg class="lane-identity" :class="{ failed: node.loadState === 'failed' }" viewBox="0 0 16 16" aria-hidden="true">
                        <path v-if="moduleBoundary" d="M6 2v12m4-12v12M2 5h4m4 6h4" />
                        <path v-else-if="symbolicMembers !== undefined" d="m2 6 6-3 6 3-6 3zm0 3 6 3 6-3M2 12l6 3 6-3" />
                        <path v-else :d="lane?.icon?.value ?? 'M4 3h8l2 2v6l-2 2H4l-2-2V5z'" />
                    </svg>
                    <input v-if="nameEdit" ref="nameInput" v-model="nameEdit.draft" class="lane-name lane-name-edit iv-graph-header-detail"
                        aria-label="Lane name" :placeholder="node.defaultDisplayName" spellcheck="false" tabindex="-1"
                        @blur="finishRename(true)" @keydown.enter="!$event.isComposing && finishRename(true)"
                        @keydown.esc.stop.prevent="finishRename(false)" />
                    <component v-else :is="lane?.openSource ? 'button' : 'span'" class="lane-title iv-graph-header-detail"
                        :class="{ 'has-secondary-label': labels.secondary, 'source-link': lane?.openSource }"
                        :type="lane?.openSource ? 'button' : undefined" :tabindex="lane?.openSource ? -1 : undefined"
                        :data-status-hint="laneStatus"
                        :data-status-shortcuts="lane?.openSource ? 'Click to open source' : lane?.rename ? 'Double-click to rename' : undefined"
                        :aria-description="lane?.openSource ? 'Open declaration in source' : laneStatus"
                        @click.exact="lane?.openSource?.()" @dblclick.exact="beginRename">
                        <span class="lane-name" :class="{ failed: node.loadState === 'failed', 'iv-text-link': lane?.openSource }">{{ labels.primary }}</span>
                        <span v-if="labels.secondary" class="lane-secondary-label" :class="{ 'iv-text-link': lane?.openSource }">{{ labels.secondary }}</span>
                    </component>
                    <span v-if="symbolicMembers !== undefined" class="member-count iv-graph-header-detail" :aria-label="memberDescription"
                        :data-status-hint="memberDescription">{{ symbolicMembers }}</span>
                    <button v-if="lane && !moduleBoundary" type="button" tabindex="-1" class="copy-debug iv-graph-header-detail"
                        data-status-hint="Copy node debug details"
                        aria-label="Copy node debug details" @click="lane.copyDebugDetails()">⧉</button>
                </div>
                <NodePorts v-if="node.address" :inline="inlineHeader" :module-boundary="moduleBoundary" share-label-space />
                <div v-else class="port-summary">
                    <span :aria-label="`${inputs.length} inputs`" :aria-description="inputSummary">→ {{ inputs.length }}</span>
                    <span :aria-label="`${outputs.length} outputs`" :aria-description="outputSummary">{{ outputs.length }} →</span>
                </div>
            </div>
            <button v-if="canExpand" type="button" tabindex="-1" class="module-footer"
                :aria-label="footerAction" :aria-expanded="expanded" :data-status-hint="footerAction"
                @click.exact.stop="editor?.toggleModule(node.address!)">
                <span class="module-footer-chevron" aria-hidden="true" />
            </button>
        </header>
        <div v-if="!moduleBoundary" class="lane-content" :class="{ 'flush-content': flushContent || !$slots.default }">
            <slot><LiveOutputMeters :mode="mode" /></slot>
        </div>
    </div>
</template>

<style scoped>
.node-lane { display: grid; grid-template-columns: var(--node-label-width, 220px) minmax(0, 1fr); grid-template-rows: minmax(0, 1fr); height: 100%; min-height: 0; container: node-lane / size; }
.lane-label {
    /* Leave space for a port row and the folded socket at the editable boundary. */
    --header-growth: clamp(0px, calc(100cqh - v-bind(editableHeight)), 25px);
    --heading-height: calc(14px + var(--header-growth) * .24);
    --module-footer-height: 0px;
    --header-content-height: calc(100cqh - var(--module-footer-height));
    position: relative; display: flex; flex-direction: column; min-width: 0; min-height: 0;
    overflow-x: clip;
    padding-block-start: calc(1px + var(--header-growth) * .28);
    padding-inline: var(--iv-graph-header-inset); border-right: 1px solid var(--vscode-panel-border);
}
.lane-label.has-module-footer { --module-footer-height: clamp(4px, 10cqh, 8px); }
.lane-header-main { display: flex; flex: 1; flex-direction: column; min-width: 0; min-height: 0; }
.module-footer {
    position: relative; pointer-events: none;
    flex: 0 0 var(--module-footer-height); min-height: 0; padding: 0; border: 0;
    margin-inline: calc(-1 * var(--iv-graph-header-inset)); cursor: pointer; background: transparent;
}
.module-footer::after {
    content: ""; position: absolute; inset-inline: 0; pointer-events: auto;
    inset-block-end: 0; height: min(6px, 100%);
}
.module-footer-chevron {
    --chevron-height: min(6px, var(--module-footer-height));
    position: absolute; inset-inline: 0; margin-inline: auto; inset-block-end: 0;
    width: clamp(13.5px, calc(100cqw - 108px), 30px);
    height: min(100cqh, clamp(calc(4px + var(--chevron-height) * 1.25), 25cqh, 18px));
}
.module-footer-chevron::before {
    content: ""; position: absolute; inset-inline: 0; margin-inline: auto;
    inset-block-end: min(4px, calc((100cqh - var(--chevron-height)) / 2));
    width: 9px; height: var(--chevron-height);
    background: var(--vscode-icon-foreground, var(--vscode-foreground));
    mask: url("../../media/codicon-chevron-down.svg") center / min(13px, calc(var(--module-footer-height) * 2.4)) no-repeat;
    transform: scale(1.5);
}
.module-footer[aria-expanded="true"] .module-footer-chevron::before {
    mask-image: url("../../media/codicon-chevron-up.svg");
}
.lane-label[data-module-footer-target] { cursor: pointer; }
.has-module-footer::after {
    content: ""; position: absolute; inset-inline: 0; inset-block-end: 0;
    height: var(--module-footer-height); border-radius: 4px; pointer-events: none;
    background: linear-gradient(to top, var(--vscode-editor-foreground, var(--vscode-foreground)), transparent);
    opacity: 0;
}
.has-module-footer[data-hovered]:not([data-module-footer-target])::after { opacity: .015; }
.has-module-footer:has(> .module-footer:is([data-hovered], [data-seam-hovered]))::after { opacity: .045; }
.lane-heading { display: flex; flex: 0 0 var(--heading-height); align-items: flex-start; gap: 6px; }
.module-child .lane-label { background-image: var(--iv-diagonal-stripes); }
.module-child .lane-heading { opacity: .74; }
.boundary-lane .lane-label {
    --boundary-tint: color-mix(in srgb, var(--vscode-editorWidget-background, var(--vscode-editor-background)) 82%, transparent);
    /* Let the parent group's inset shadow show through at the left edge. */
    background-image: var(--iv-diagonal-stripes), linear-gradient(to right, transparent, var(--boundary-tint) 12px);
    background-clip: padding-box;
    box-shadow: inset 0 1px var(--vscode-panel-border);
}
.boundary-lane::after {
    content: ""; pointer-events: none;
    background: linear-gradient(to right, color-mix(in srgb, var(--vscode-panel-border) 26%, transparent), transparent 48px);
}
.lane-identity { fill: none; stroke: currentColor; stroke-width: 1.25;
    stroke-linecap: round; stroke-linejoin: round; }
.lane-identity { flex: 0 0 auto; width: min(13px, var(--header-content-height));
    height: min(13px, var(--header-content-height)); color: var(--vscode-icon-foreground); }
.member-count { flex: 0 0 auto; color: var(--vscode-descriptionForeground); font-size: min(11px, calc(100cqh - 3px));
    font-variant-numeric: tabular-nums; line-height: 1.1; }
.lane-label :deep(.node-ports) { flex: 1; min-height: 0; margin-top: calc(var(--header-growth) * .2); }
/* A folded socket overlaps the footer only where that socket takes the pointer. */
.has-module-footer :deep(.node-ports:has(.port-slot.cropped)) { z-index: 1; }
.port-summary { display: flex; gap: 16px; margin-top: 6px; font-size: .9em; color: var(--vscode-descriptionForeground); }
.inline-header .lane-label {
    --header-content-height: var(--graph-lane-height);
    padding-block-start: 0;
}
.inline-header .lane-header-main { flex-direction: row; align-items: center; gap: 4px; }
.inline-header .lane-heading { flex: 1; min-width: var(--inline-title-width); overflow: hidden; align-items: center; }
.inline-header .lane-label :deep(.node-ports) {
    --port-slot-height: min(22px, var(--header-content-height));
    margin-top: 0; flex: 0 0 var(--inline-port-width); min-width: 0; min-height: 0; height: var(--port-slot-height);
}
.inline-header .module-footer {
    position: absolute; inset-inline: 0; inset-block-end: 0; margin: 0; height: var(--module-footer-height);
}
.inline-header .module-footer::after { pointer-events: none; }
.inline-header .module-footer-chevron {
    inset-inline-start: auto; inset-inline-end: calc(var(--iv-graph-header-inset) + var(--inline-port-column-width));
    width: var(--inline-port-gap); height: min(18px, 100cqh);
}
.inline-header .port-summary { display: none; }
.lane-title { flex: 1; display: flex; align-items: baseline; gap: 6px; min-width: 0; }
.source-link { padding: 0; border: 0; background: transparent; font: inherit; text-align: start; cursor: pointer; }
.lane-name, .lane-secondary-label { min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    line-height: 1; }
.lane-name {
    font-size: min(calc(100cqh - 4px), var(--heading-height), var(--vscode-font-size, 13px));
    --iv-text-link-color: var(--iv-lane-title-color); font-weight: 500; }
.lane-name:not(.iv-text-link) { color: var(--iv-text-link-color); }
.has-secondary-label .lane-name { max-width: 68%; }
.lane-secondary-label { font-size: min(calc(100cqh - 4px), var(--heading-height), calc(var(--vscode-font-size, 13px) * .85)); }
.lane-secondary-label:not(.iv-text-link) { color: var(--vscode-descriptionForeground); }
.lane-name.failed { --iv-text-link-color: var(--vscode-errorForeground, #f14c4c); }
.lane-name-edit { flex: 1; box-sizing: border-box; width: 0; padding: 0; border: 0; outline: none;
    background: var(--vscode-input-background); color: var(--vscode-input-foreground); }
.copy-debug { flex: 0 0 20px; height: var(--heading-height); padding: 0; border: 0; border-radius: 3px;
    color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; font-size: min(16px, var(--heading-height)); line-height: 1; }
.inline-header .copy-debug { height: min(100cqh, var(--heading-height)); font-size: min(100cqh, 14px, var(--heading-height)); }
.copy-debug[data-hovered] { background: var(--vscode-toolbar-hoverBackground); }
@container graph-header (max-width: 92px) {
    .lane-identity.failed { color: var(--vscode-errorForeground, #f14c4c); }
}
.lane-content { min-width: 0; min-height: 0; overflow: hidden; padding: clamp(0px, calc(12.5cqh - 2px), 4px) 0; box-sizing: border-box;
    content-visibility: auto; }
.lane-content.flush-content { padding: 0; }
</style>

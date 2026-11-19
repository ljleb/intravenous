<script setup lang="ts">
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import { computed, nextTick, ref } from "vue";
import { nodeDisplayName, portDescription } from "./nodeSummary";
import { editableHeightPx, type NodeDisplayMode } from "./displayMode";
import NodePorts from "./NodePorts.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";

const props = defineProps<{ mode?: NodeDisplayMode; flushContent?: boolean }>();

const node = useNode();
const lane = useLane();
const editableHeight = `${editableHeightPx}px`;
const inlineHeader = computed(() => props.mode === "mini" || props.mode === "nano"
    || (props.mode === "compact" && (lane?.regionHeight.value ?? Infinity) < editableHeightPx));
const inputs = computed(() => node.value.ports.filter((port) => port.direction === "input"));
const outputs = computed(() => node.value.ports.filter((port) => port.direction === "output"));
const inputSummary = computed(() => inputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const outputSummary = computed(() => outputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const laneStatus = computed(() => node.value.loadState === "waitingForDefinition" ? "Waiting for the node definition"
    : node.value.loadState === "waitingForGraph" ? "Waiting for graph realization"
        : node.value.errorMessage || node.value.definitionId);
const name = computed(() => nodeDisplayName(node.value));
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
                <input v-if="nameEdit" ref="nameInput" v-model="nameEdit.draft" class="lane-name lane-name-edit"
                    aria-label="Lane name" :placeholder="node.defaultDisplayName" spellcheck="false" tabindex="-1"
                    @blur="finishRename(true)" @keydown.enter="!$event.isComposing && finishRename(true)"
                    @keydown.esc.stop.prevent="finishRename(false)" />
                <span v-else class="lane-name" :class="{ failed: node.loadState === 'failed' }"
                    :data-status-hint="laneStatus" :data-status-shortcuts="lane?.rename ? 'Double-click to rename' : undefined"
                    :aria-description="laneStatus" @dblclick="beginRename">
                    {{ name }}
                </span>
                <button v-if="lane" type="button" tabindex="-1" class="copy-debug" data-status-hint="Copy node debug details"
                    aria-label="Copy node debug details" @click="lane.copyDebugDetails()">⧉</button>
            </div>
            <NodePorts v-if="node.address" :inline="inlineHeader" />
            <div v-else class="port-summary">
                <span :aria-label="`${inputs.length} inputs`" :aria-description="inputSummary">→ {{ inputs.length }}</span>
                <span :aria-label="`${outputs.length} outputs`" :aria-description="outputSummary">{{ outputs.length }} →</span>
            </div>
        </header>
        <div class="lane-content" :class="{ 'flush-content': flushContent || !$slots.default }">
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
    display: flex; flex-direction: column; min-width: 0; min-height: 0;
    padding: calc(1px + var(--header-growth) * .28) 12px 0; border-right: 1px solid var(--vscode-panel-border);
}
.lane-heading { display: flex; flex: 0 0 var(--heading-height); align-items: flex-start; gap: 6px; }
.lane-label :deep(.node-ports) { flex: 1; min-height: 32px; margin-top: calc(var(--header-growth) * .2); }
.port-summary { display: flex; gap: 16px; margin-top: 6px; font-size: .9em; color: var(--vscode-descriptionForeground); }
.inline-header .lane-label { flex-direction: row; align-items: center; gap: 4px; padding: 0 4px; }
.inline-header .lane-heading { flex: 1; min-width: 0; overflow: hidden; align-items: center; }
.inline-header .lane-label :deep(.node-ports) {
    --port-slot-height: min(22px, 100cqh);
    margin-top: 0; flex: 0 1 60px; min-width: 0; min-height: 0; height: var(--port-slot-height);
}
.inline-header .port-summary { display: none; }
.lane-name { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    font-size: min(calc(100cqh - 4px), var(--heading-height), var(--vscode-font-size, 13px)); line-height: 1;
    color: var(--iv-lane-title-color); font-weight: 500; }
.lane-name.failed { color: var(--vscode-errorForeground, #f14c4c); }
.lane-name-edit { box-sizing: border-box; width: 0; padding: 0; border: 0; outline: none;
    background: var(--vscode-input-background); color: var(--vscode-input-foreground); }
.copy-debug { flex: 0 0 20px; height: var(--heading-height); padding: 0; border: 0; border-radius: 3px;
    color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; font-size: min(16px, var(--heading-height)); line-height: 1; }
.inline-header .copy-debug { height: min(100cqh, var(--heading-height)); font-size: min(100cqh, 14px, var(--heading-height)); }
.copy-debug[data-hovered] { background: var(--vscode-toolbar-hoverBackground); }
.lane-content { min-width: 0; min-height: 0; overflow: hidden; padding: clamp(0px, calc(12.5cqh - 2px), 4px) 0; box-sizing: border-box; }
.lane-content.flush-content { padding: 0; }
</style>

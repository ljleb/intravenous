<script setup lang="ts">
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import { computed } from "vue";
import { nodeDisplayName, portDescription } from "./nodeSummary";
import type { NodeDisplayMode } from "./displayMode";
import NodePorts from "./NodePorts.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";

defineProps<{ mode?: NodeDisplayMode }>();

const node = useNode();
const lane = useLane();
const inputs = computed(() => node.value.ports.filter((port) => port.direction === "input"));
const outputs = computed(() => node.value.ports.filter((port) => port.direction === "output"));
const inputSummary = computed(() => inputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const outputSummary = computed(() => outputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const laneStatus = computed(() => node.value.loadState === "waitingForDefinition" ? "Waiting for the node definition"
    : node.value.loadState === "waitingForGraph" ? "Waiting for graph realization"
        : node.value.errorMessage || node.value.definitionId);
</script>

<template>
    <div class="node-lane" :data-mode="mode">
        <header class="lane-label">
            <div class="lane-heading">
                <span class="lane-name" :class="{ failed: node.loadState === 'failed' }"
                    :data-status-hint="laneStatus" :aria-description="laneStatus">
                    {{ nodeDisplayName(node) }}
                </span>
                <button v-if="lane" type="button" class="copy-debug" data-status-hint="Copy node debug details"
                    aria-label="Copy node debug details" @click="lane.copyDebugDetails()">⧉</button>
            </div>
            <NodePorts v-if="node.address" />
            <div v-else class="port-summary">
                <span :aria-label="`${inputs.length} inputs`" :aria-description="inputSummary">→ {{ inputs.length }}</span>
                <span :aria-label="`${outputs.length} outputs`" :aria-description="outputSummary">{{ outputs.length }} →</span>
            </div>
        </header>
        <div class="lane-content" :class="{ 'meter-content': !$slots.default }"><slot><LiveOutputMeters :mode="mode" /></slot></div>
    </div>
</template>

<style scoped>
.node-lane { display: grid; grid-template-columns: var(--node-label-width, 220px) minmax(0, 1fr); grid-template-rows: minmax(0, 1fr); height: 100%; min-height: 0; container: node-lane / size; }
.lane-label { display: flex; flex-direction: column; min-width: 0; min-height: 0; padding: 8px 12px; border-right: 1px solid var(--vscode-panel-border); }
.lane-heading { display: flex; flex: 0 0 auto; align-items: flex-start; gap: 6px; }
.lane-label :deep(.node-ports) { flex: 1; min-height: 22px; }
.port-summary { display: flex; gap: 16px; margin-top: 6px; font-size: .9em; color: var(--vscode-descriptionForeground); }
/* Header geometry follows its available space, independently of the
   presentation's editable/mini/nano content contract. */
@container node-lane (max-height: 56px) {
    .lane-label { flex-direction: row; align-items: center; gap: 4px; padding: 0 4px; }
    .lane-heading { flex: 1; min-width: 0; overflow: hidden; align-items: center; }
    .lane-label :deep(.node-ports) {
        --port-slot-height: min(22px, 100cqh);
        margin-top: 0; flex: 0 1 130px; min-width: 0; min-height: 0; height: var(--port-slot-height);
    }
    .port-summary { display: none; }
}
.lane-name { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: min(calc(100cqh - 4px), var(--vscode-font-size, 13px)); line-height: 1; }
.lane-name.failed { color: var(--vscode-errorForeground, #f14c4c); }
.copy-debug { flex: 0 0 20px; height: 20px; padding: 0; border: 0; border-radius: 3px; color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; font-size: 16px; line-height: 20px; }
.copy-debug[data-hovered] { background: var(--vscode-toolbar-hoverBackground); }
.copy-debug:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
.lane-content { min-width: 0; min-height: 0; overflow: hidden; padding: clamp(0px, calc(12.5cqh - 2px), 4px) 0; box-sizing: border-box; }
.lane-content.meter-content { padding: 0; }
</style>

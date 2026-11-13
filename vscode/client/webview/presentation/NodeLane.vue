<script setup lang="ts">
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import { computed } from "vue";
import { nodeDisplayName, portDescription } from "./nodeSummary";
import type { NodeDisplayMode } from "./displayMode";

defineProps<{ mode?: NodeDisplayMode }>();

const node = useNode();
const lane = useLane();
const inputs = computed(() => node.value.ports.filter((port) => port.direction === "input"));
const outputs = computed(() => node.value.ports.filter((port) => port.direction === "output"));
const inputSummary = computed(() => inputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
const outputSummary = computed(() => outputs.value.map((port) => `${port.name}: ${portDescription(port)}`).join('\n'));
</script>

<template>
    <div class="node-lane" :data-mode="mode">
        <header class="lane-label">
            <div class="lane-heading">
                <span class="lane-name" :class="{ failed: node.loadState === 'failed' }"
                    :title="node.loadState === 'waitingForDefinition' ? 'Waiting for the node definition'
                        : node.loadState === 'waitingForGraph' ? 'Waiting for graph realization'
                        : node.errorMessage || node.definitionId">
                    {{ nodeDisplayName(node) }}
                </span>
                <button v-if="lane" type="button" class="copy-debug" title="Copy node debug details"
                    aria-label="Copy node debug details" @click="lane.copyDebugDetails()">⧉</button>
            </div>
            <div class="port-summary">
                <span :title="inputSummary" :aria-label="`${inputs.length} inputs`">→ {{ inputs.length }}</span>
                <span :title="outputSummary" :aria-label="`${outputs.length} outputs`">{{ outputs.length }} →</span>
            </div>
        </header>
        <div class="lane-content"><slot /></div>
    </div>
</template>

<style scoped>
.node-lane { display: grid; grid-template-columns: var(--node-label-width, 220px) minmax(0, 1fr); height: 100%; min-height: 0; }
.lane-label { min-width: 0; padding: 8px 12px; border-right: 1px solid var(--vscode-panel-border); }
.lane-heading { display: flex; align-items: flex-start; gap: 6px; }
.port-summary { display: flex; gap: 16px; margin-top: 6px; font-size: .9em; color: var(--vscode-descriptionForeground); }
.node-lane[data-mode="nano"] .lane-label { padding-top: 2px; padding-bottom: 2px; }
.node-lane[data-mode="nano"] .port-summary, .node-lane[data-mode="compact"] .port-summary { display: none; }
.lane-name { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.lane-name.failed { color: var(--vscode-errorForeground, #f14c4c); }
.copy-debug { flex: 0 0 20px; height: 20px; padding: 0; border: 0; border-radius: 3px; color: var(--vscode-icon-foreground); background: transparent; cursor: pointer; font-size: 16px; line-height: 20px; }
.copy-debug:hover { background: var(--vscode-toolbar-hoverBackground); }
.copy-debug:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
.lane-content { min-width: 0; min-height: 0; overflow: auto; padding: 4px 0; }
</style>

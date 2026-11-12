<script setup lang="ts">
import { computed } from "vue";

import arrowRightSvg from "../../media/arrow_right.svg?raw";
import { portDescription, portTypeSummary } from "./nodeSummary";
import type { SemanticPort } from "./semanticNode";

const props = defineProps<{
    port: SemanticPort;
    detailed: boolean;
}>();

const arrowIcon = arrowRightSvg.replace(/fill="[^"]*"/g, 'fill="currentColor"');

const description = computed(() => props.detailed
    ? portDescription(props.port)
    : portTypeSummary(props.port));
const active = computed(() => props.port.binding !== "none"
    || props.port.connectivity !== "disconnected");
</script>

<template>
    <div
        class="port-row"
        :data-direction="port.direction"
        :data-active="active"
        :title="`${port.name} • ${portDescription(port)}`"
    >
        <span class="port-icon" :class="{ mirrored: port.direction === 'output' }" v-html="arrowIcon" />
        <span class="port-name">{{ port.name }}</span>
        <span class="port-description">{{ description }}</span>
    </div>
</template>

<style scoped>
.port-row {
    display: flex;
    align-items: center;
    gap: 6px;
    min-height: 22px;
    padding: 0 8px 0 28px;
    white-space: nowrap;
    overflow: hidden;
}

.port-row:hover {
    background: var(--vscode-list-hoverBackground);
}

.port-icon {
    display: inline-flex;
    width: 16px;
    height: 16px;
    flex: 0 0 16px;
    opacity: 0.45;
}

.port-icon :deep(svg) {
    width: 16px;
    height: 16px;
}

.port-icon.mirrored :deep(svg) {
    transform: scaleX(-1);
}

.port-row[data-direction="input"] .port-icon {
    color: var(--vscode-terminal-ansiYellow);
}

.port-row[data-direction="output"] .port-icon {
    color: var(--vscode-terminal-ansiBlue);
}

.port-row[data-active="true"] .port-icon {
    opacity: 0.95;
}

.port-name {
    flex: 0 1 auto;
    overflow: hidden;
    text-overflow: ellipsis;
}

.port-description {
    flex: 0 1 auto;
    overflow: hidden;
    text-overflow: ellipsis;
    color: var(--vscode-descriptionForeground);
}
</style>

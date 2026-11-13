<script setup lang="ts">
import { computed, ref } from "vue";

import chevronDownSvg from "../../media/chevron_down.svg?raw";
import chevronLeftSvg from "../../media/chevron_left.svg?raw";
import mergedNodeSvg from "../../media/merged_node.svg?raw";
import singleNodeSvg from "../../media/single_node.svg?raw";
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";
import { nodeDescription, nodeDisplayName, portDescription, portGroups, portKey } from "./nodeSummary";
import PortRow from "./PortRow.vue";

// The presentation every node has. It works from the node's reflected ports
// alone and shows every port in every mode: compact and nano summarize them,
// mini lists them, and editable lists them with their full description and the
// concrete members.
defineProps<{
    mode: NodeDisplayMode;
}>();

const node = useNode();

const currentColor = (svg: string) => svg.replace(/fill="[^"]*"/g, 'fill="currentColor"');
const collapsedIcon = currentColor(chevronLeftSvg);
const expandedIcon = currentColor(chevronDownSvg);

const icon = computed(() => node.value.memberCount > 1 ? mergedNodeSvg : singleNodeSvg);
const description = computed(() => nodeDescription(node.value));
const inputCount = computed(() => node.value.ports.filter((port) => port.direction === "input").length);
const outputCount = computed(() => node.value.ports.length - inputCount.value);
const groups = computed(() => portGroups(node.value.ports));

const membersExpanded = ref(false);
const expandedMembers = ref(new Set<number>());

function toggleMember(index: number): void {
    const next = new Set(expandedMembers.value);
    if (!next.delete(index)) {
        next.add(index);
    }
    expandedMembers.value = next;
}
</script>

<template>
    <div class="generic-node" :class="`mode-${mode}`">
        <div class="node-header" :title="`${node.kind} • ${description}`">
            <span class="node-icon" v-html="icon" />
            <span class="node-kind" :class="{ failed: node.loadState === 'failed' }" :title="node.errorMessage">{{ nodeDisplayName(node) }}</span>
            <span v-if="mode === 'compact'" class="node-description">
                {{ inputCount }} in · {{ outputCount }} out
            </span>
            <span v-else class="node-description">{{ description }}</span>
        </div>

        <div v-if="mode === 'nano'" class="port-strip">
            <span
                v-for="port in node.ports"
                :key="portKey(port)"
                class="port-dot"
                :data-direction="port.direction"
                :data-connectivity="port.connectivity"
                :title="`${port.direction} ${port.name} • ${portDescription(port)}`"
            />
        </div>

        <template v-else-if="mode === 'mini'">
            <PortRow
                v-for="port in node.ports"
                :key="portKey(port)"
                :port="port"
                :detailed="false"
            />
        </template>

        <template v-else-if="mode === 'editable'">
            <div v-for="group in groups" :key="group.label" class="port-group">
                <div class="group-label">
                    {{ group.label }}
                    <span class="group-count">{{ group.ports.length }}</span>
                </div>
                <PortRow
                    v-for="port in group.ports"
                    :key="portKey(port)"
                    :port="port"
                    detailed
                />
            </div>

            <div v-if="node.members.length > 0" class="members">
                <button
                    type="button"
                    class="disclosure-row"
                    @click="membersExpanded = !membersExpanded"
                >
                    <span
                        class="disclosure"
                        :class="{ collapsed: !membersExpanded }"
                        v-html="membersExpanded ? expandedIcon : collapsedIcon"
                    />
                    concrete nodes
                    <span class="group-count">{{ node.members.length }}</span>
                </button>
                <template v-if="membersExpanded">
                    <div v-for="member in node.members" :key="member.index" class="member">
                        <button
                            type="button"
                            class="disclosure-row member-row"
                            @click="toggleMember(member.index)"
                        >
                            <span
                                class="disclosure"
                                :class="{ collapsed: !expandedMembers.has(member.index) }"
                                v-html="expandedMembers.has(member.index) ? expandedIcon : collapsedIcon"
                            />
                            {{ member.kind || "concrete" }}
                            <span class="group-count">concrete {{ member.index }}</span>
                        </button>
                        <template v-if="expandedMembers.has(member.index)">
                            <PortRow
                                v-for="port in member.ports"
                                :key="portKey(port)"
                                :port="port"
                                detailed
                                class="member-port"
                            />
                        </template>
                    </div>
                </template>
            </div>
        </template>
    </div>
</template>

<style scoped>
.node-kind.failed { color: var(--vscode-errorForeground, #f14c4c); }
.generic-node {
    border-bottom: 1px solid var(--vscode-sideBar-border, transparent);
    padding-bottom: 2px;
}

.node-header {
    display: flex;
    align-items: center;
    gap: 6px;
    min-height: 22px;
    padding: 0 8px;
    white-space: nowrap;
    overflow: hidden;
}

.node-icon {
    display: inline-flex;
    width: 16px;
    height: 16px;
    flex: 0 0 16px;
}

.node-kind {
    flex: 0 1 auto;
    overflow: hidden;
    text-overflow: ellipsis;
}

.node-description,
.group-count {
    flex: 0 1 auto;
    overflow: hidden;
    text-overflow: ellipsis;
    color: var(--vscode-descriptionForeground);
}

.port-strip {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
    padding: 2px 8px 4px 30px;
}

.port-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    opacity: 0.45;
}

.port-dot[data-direction="input"] {
    background: var(--vscode-terminal-ansiYellow);
}

.port-dot[data-direction="output"] {
    background: var(--vscode-terminal-ansiBlue);
}

.port-dot[data-connectivity="connected"],
.port-dot[data-connectivity="mixed"] {
    opacity: 0.95;
}

.group-label {
    display: flex;
    gap: 6px;
    min-height: 22px;
    align-items: center;
    padding: 0 8px 0 24px;
}

.disclosure-row {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    min-height: 22px;
    padding: 0 8px 0 24px;
    border: 0;
    background: transparent;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
}

.disclosure-row:hover {
    background: var(--vscode-list-hoverBackground);
}

.disclosure-row:focus-visible {
    outline: 1px solid var(--vscode-focusBorder);
    outline-offset: -1px;
}

.member-row {
    padding-left: 42px;
}

.disclosure {
    display: inline-flex;
    width: 12px;
    height: 12px;
    flex: 0 0 12px;
    color: var(--vscode-descriptionForeground);
}

.disclosure :deep(svg) {
    width: 12px;
    height: 12px;
}

/* chevron_left.svg mirrored points right. */
.disclosure.collapsed :deep(svg) {
    transform: scaleX(-1);
}

.member .member-port {
    padding-left: 60px;
}
</style>

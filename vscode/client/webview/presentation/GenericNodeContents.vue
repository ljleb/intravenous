<script setup lang="ts">
import { computed, ref } from "vue";

import mergedNodeSvg from "../../media/merged_node.svg?raw";
import singleNodeSvg from "../../media/single_node.svg?raw";
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";
import { nodeDescription, nodeDisplayName, portDescription, portGroups, portKey } from "./nodeSummary";
import PortRow from "./PortRow.vue";
import DisclosureRow from "./DisclosureRow.vue";

// The presentation every node has. It works from the node's reflected ports
// alone and shows every port in every mode: compact and nano summarize them,
// mini lists them, and editable lists them with their full description and the
// concrete members.
defineProps<{
    mode: NodeDisplayMode;
}>();

const node = useNode();

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
        <div class="node-header" :data-status-hint="`${node.kind} · ${description}`">
            <span class="node-icon" v-html="icon" />
            <span class="node-kind" :class="{ failed: node.loadState === 'failed' }"
                :data-status-hint="node.errorMessage" :aria-description="node.errorMessage">{{ nodeDisplayName(node) }}</span>
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
                :data-kind="port.kind"
                :data-connectivity="port.connectivity"
                :data-status-hint="`${port.direction} ${port.name} · ${portDescription(port)}`"
                :aria-label="`${port.direction} ${port.name} · ${portDescription(port)}`"
                role="img"
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
                <DisclosureRow :expanded="membersExpanded" @click="membersExpanded = !membersExpanded">
                    concrete nodes
                    <template #detail>{{ node.members.length }}</template>
                </DisclosureRow>
                <template v-if="membersExpanded">
                    <div v-for="member in node.members" :key="member.index" class="member">
                        <DisclosureRow class="member-row" :expanded="expandedMembers.has(member.index)" @click="toggleMember(member.index)">
                            {{ member.kind || "concrete" }}
                            <template #detail>concrete {{ member.index }}</template>
                        </DisclosureRow>
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
    background: var(--iv-port-input);
}

.port-dot[data-direction="output"] {
    background: var(--iv-port-output);
}
.port-dot[data-kind="event"] { border-radius: 2px; }

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

.member-row { --disclosure-indent: 42px; }

.member .member-port {
    padding-left: 60px;
}
</style>

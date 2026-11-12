<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";

import type { VirtualNode } from "../../src/graphModel";
import { effectiveDisplayMode, type NodeDisplayMode, type NodeViewChoice } from "./displayMode";
import { provideNode } from "./nodeContext";
import { presentations } from "./presentations";
import { semanticNodeFromVirtualNode } from "./semanticNode";

// Hosts one node: selects its presentation, provides its semantic node, and
// supplies the one display mode that presentation receives.
const props = defineProps<{
    node: VirtualNode;
    viewChoice: NodeViewChoice;
}>();

const semanticNode = computed(() => semanticNodeFromVirtualNode(props.node));
provideNode(semanticNode);

const selection = computed(() => presentations.select(semanticNode.value.definitionId));

const root = ref<HTMLElement | null>(null);
const width = ref<number | null>(null);
const mode = ref<NodeDisplayMode>(props.viewChoice);

watch(
    [width, () => props.viewChoice],
    ([availableWidth, viewChoice]) => {
        mode.value = effectiveDisplayMode(availableWidth, viewChoice, mode.value);
    },
    { immediate: true },
);

let observer: ResizeObserver | null = null;
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        const entry = entries[entries.length - 1];
        if (entry) {
            width.value = entry.contentRect.width;
        }
    });
    if (root.value) {
        observer.observe(root.value);
    }
});
onBeforeUnmount(() => {
    observer?.disconnect();
    observer = null;
});
</script>

<template>
    <div
        ref="root"
        class="presentation-host"
        :data-mode="mode"
        :data-presentation="selection.presentationId"
    >
        <component :is="selection.component" :mode="mode" />
    </div>
</template>

<style scoped>
.presentation-host {
    display: block;
    min-width: 0;
}
</style>

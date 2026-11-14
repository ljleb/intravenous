<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";

import { effectiveDisplayMode, type NodeDisplayMode, type NodeViewChoice } from "./displayMode";
import { provideNode } from "./nodeContext";
import { presentations } from "./presentations";
import type { SemanticNode } from "./semanticNode";
import { provideLane } from "./laneContext";

// Hosts one node: selects its presentation, provides its semantic node, and
// supplies the one display mode that presentation receives.
const props = defineProps<{
    node: SemanticNode;
    viewChoice?: NodeViewChoice;
    regionHeight?: number;
    scale?: number;
    lane?: boolean;
}>();
const emit = defineEmits<{ copyDebugDetails: [] }>();
provideLane(props.lane ? { copyDebugDetails: () => emit("copyDebugDetails") } : null);

const semanticNode = computed(() => props.node);
provideNode(semanticNode);

const selection = computed(() => presentations.select(semanticNode.value.definitionId));

const root = ref<HTMLElement | null>(null);
const width = ref<number | null>(null);
const mode = ref<NodeDisplayMode>(props.viewChoice ?? "mini");

watch(
    [width, () => props.viewChoice, () => props.scale ?? 1, () => props.regionHeight ?? null],
    ([availableWidth, viewChoice, scale, availableHeight]) => {
        mode.value = effectiveDisplayMode(availableWidth === null ? null : availableWidth * scale,
            viewChoice ?? null, mode.value, availableHeight);
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
        :style="regionHeight === undefined ? undefined : { height: `${regionHeight}px`, overflow: 'auto' }"
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

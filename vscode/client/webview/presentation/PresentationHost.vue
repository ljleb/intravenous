<script setup lang="ts">
import { computed, ref, toRef, watch } from "vue";

import { effectiveDisplayMode, type NodeDisplayMode, type NodeViewChoice } from "./displayMode";
import { provideNode } from "./nodeContext";
import { selectPresentation } from "./presentations";
import type { SemanticNode } from "./semanticNode";
import { provideLane } from "./laneContext";
import { useNodeEventMessaging } from "./nodeEventContext";
import type { NodeEvent } from "../../src/nodeEvents";
import { presentationStateKey } from "../../src/graphScope";
import { useOptionalGraphEditor } from "../graph/context";
import { providePresentationState } from "./presentationStateContext";
import { provideObservationTier } from "./observationContext";
import NodeLane from "./NodeLane.vue";
import { usePresentationSize } from "./presentationSize";

// Hosts one node: selects its presentation, provides its semantic node, and
// supplies the one display mode that presentation receives.
const props = defineProps<{
    node: SemanticNode;
    viewChoice?: NodeViewChoice;
    regionHeight?: number;
    scale?: number;
    lane?: boolean;
    moduleBoundary?: boolean;
    openSource?: () => void;
    renameLane?: (name: string, previousName: string) => void;
}>();
const emit = defineEmits<{ copyDebugDetails: [] }>();
provideLane(props.lane || props.moduleBoundary ? {
    copyDebugDetails: () => emit("copyDebugDetails"),
    get openSource() { return props.openSource; },
    get rename() { return props.renameLane; },
    regionHeight: toRef(() => props.regionHeight),
    icon: computed(() => selection.value.icon),
} : null);

const nodeEvents = useNodeEventMessaging();
const semanticNode = computed(() => {
    const node = props.node;
    return { ...node, send: (event: NodeEvent) => nodeEvents.send(node.address, event),
        gesture: () => nodeEvents.gesture(node.address) };
});
provideNode(semanticNode);

const selection = computed(() => selectPresentation(semanticNode.value.definitionId));
const editor = useOptionalGraphEditor();
const stateKey = computed(() => props.node.address ? presentationStateKey(props.node.address, selection.value.presentationId) : null);
providePresentationState(({ serialize, restore }) => {
    const key = stateKey.value;
    restore(editor && key !== null ? editor.readPresentationState(key) : undefined);
    if (!editor || key === null) return () => {};
    watch(() => JSON.stringify(editor.readPresentationState(key) ?? null), (serialized) => {
        if (serialized !== JSON.stringify(serialize())) restore(JSON.parse(serialized) ?? undefined);
    });
    return (saveNow = true) => {
        editor.presentationStateChanged(key, serialize);
        if (saveNow) editor.save();
    };
});

const root = ref<HTMLElement | null>(null);
provideObservationTier(root);
const size = usePresentationSize(root);
const width = computed(() => size.value?.width ?? null);
const mode = ref<NodeDisplayMode>(props.viewChoice ?? "mini");

watch(
    [width, () => props.viewChoice, () => props.scale ?? 1, () => props.regionHeight ?? null],
    ([availableWidth, viewChoice, scale, availableHeight]) => {
        mode.value = effectiveDisplayMode(availableWidth === null ? null : availableWidth * scale,
            viewChoice ?? null, mode.value, availableHeight);
    },
    { immediate: true },
);

</script>

<template>
    <div
        ref="root"
        class="presentation-host"
        :data-mode="mode"
        :data-presentation="moduleBoundary ? undefined : selection.presentationId"
        :style="regionHeight === undefined ? undefined : { height: `${regionHeight}px`, overflow: 'hidden' }"
    >
        <NodeLane v-if="moduleBoundary" :mode="mode" module-boundary />
        <component v-else :is="selection.component" :key="`${stateKey ?? selection.presentationId}:${selection.serial}`"
            :mode="mode" v-bind="selection.program ? { program: selection.program } : {}" />
    </div>
</template>

<style scoped>
.presentation-host {
    display: block;
    min-width: 0;
}
</style>

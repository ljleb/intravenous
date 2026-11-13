<script setup lang="ts">
import type { NodeDisplayMode } from "./displayMode";
import { useNode } from "./nodeContext";
import { useLane } from "./laneContext";
import GenericNodeContents from "./GenericNodeContents.vue";
import NodeLane from "./NodeLane.vue";
import ManualInputs from "./ManualInputs.vue";
import TimelineScopePresentation from "../graph/TimelineScopePresentation.vue";

defineProps<{ mode: NodeDisplayMode }>();
const node = useNode();
const lane = useLane();
</script>

<template>
    <TimelineScopePresentation v-if="node.scope" />
    <NodeLane v-else-if="lane" :mode="mode"><ManualInputs v-if="mode === 'mini' || mode === 'editable'" /></NodeLane>
    <GenericNodeContents v-else :mode="mode" />
</template>

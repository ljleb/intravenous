<script setup lang="ts">
import { computed } from "vue";
import { useNode } from "./nodeContext";
import ManualInput from "./ManualInput.vue";
const node = useNode();
const inputs = computed(() => node.value.ports.filter((entry) => entry.direction === "input"
    && entry.kind === "continuous" && entry.manualControl));
</script>
<template>
    <div v-if="inputs.length" class="manual-inputs" @pointerdown.stop>
        <ManualInput v-for="port in inputs" :key="port.name" :name="port.name" />
    </div>
</template>
<style scoped>
.manual-inputs { display: flex; flex-wrap: wrap; align-items: flex-start; align-content: flex-start; }
</style>

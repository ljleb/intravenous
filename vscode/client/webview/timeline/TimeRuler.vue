<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { timelineTicks, type TimelineWindow } from "./geometry";
import RulerGraduations from "./RulerGraduations.vue";

// A presentation can use a time ruler without being a graph or a lane.
// It makes no assumption about the node's number or kinds of output ports.
const props = defineProps<{ window: TimelineWindow }>();
const root = ref<HTMLElement | null>(null);
const width = ref(0);
const ticks = computed(() => timelineTicks(props.window, width.value));
let observer: ResizeObserver | null = null;
onMounted(() => {
    observer = new ResizeObserver((entries) => {
        const entry = entries[entries.length - 1];
        if (entry) width.value = entry.contentRect.width;
    });
    if (root.value) observer.observe(root.value);
});
onBeforeUnmount(() => observer?.disconnect());
</script>

<template>
    <div ref="root" class="time-ruler" aria-label="Time in seconds">
        <RulerGraduations :ticks="ticks" />
    </div>
</template>

<style scoped>
.time-ruler { position: relative; min-width: 0; overflow: hidden; }
</style>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { timelineTicks, type TimelineWindow } from "./geometry";

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
        <div v-for="tick in ticks" :key="tick.seconds" class="time-tick" :style="{ left: `${tick.x}px` }">
            {{ tick.label }}
        </div>
    </div>
</template>

<style scoped>
.time-ruler { position: relative; min-width: 0; overflow: hidden; }
.time-tick { position: absolute; top: 0; bottom: 0; border-left: 1px solid var(--vscode-panel-border); padding: 8px 5px; white-space: nowrap; font-variant-numeric: tabular-nums; }
</style>

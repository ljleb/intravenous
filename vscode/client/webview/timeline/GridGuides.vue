<script setup lang="ts" generic="Mark extends { depth: number }">
defineProps<{
    marks: readonly Mark[];
    strengths?: ReadonlyMap<number, number>;
    markKey: (mark: Mark) => number | string;
    position: (mark: Mark) => number;
    axis?: "x" | "y";
    start?: (mark: Mark) => string | number;
    end?: string | number;
}>();
</script>

<template>
    <line v-for="mark in marks" :key="markKey(mark)" class="iv-grid-guide"
        :style="{ '--iv-grid-strength': strengths?.get(mark.depth) }"
        :x1="axis === 'y' ? start?.(mark) ?? 0 : position(mark)"
        :x2="axis === 'y' ? end ?? '100%' : position(mark)"
        :y1="axis === 'y' ? position(mark) : start?.(mark) ?? 0"
        :y2="axis === 'y' ? position(mark) : end ?? '100%'"
        vector-effect="non-scaling-stroke" />
</template>

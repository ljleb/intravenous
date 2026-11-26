<script setup lang="ts">
import { computed, ref } from "vue";
import { usePresentationSize } from "./presentationSize";

const props = defineProps<{ name: string; shortName?: string }>();
const label = ref<HTMLElement | null>(null);
const fullText = ref<HTMLElement | null>(null);
const available = usePresentationSize(computed(() => props.shortName ? label.value : null));
const full = usePresentationSize(fullText);
const useShort = computed(() => !!props.shortName && available.value !== undefined
    && full.value !== undefined && full.value.width > available.value.width);
</script>

<template>
    <span ref="label" class="port-label">
        <span v-if="shortName" class="port-label-text" :class="{ hidden: useShort }">
            <span ref="fullText" class="port-label-measure">{{ name }}</span>
        </span>
        <span v-else class="port-label-text">{{ name }}</span>
        <span v-if="useShort" class="port-label-text short" aria-hidden="true">{{ shortName }}</span>
    </span>
</template>

<style scoped>
.port-label { display: block; min-width: 0; position: relative; }
.port-label-text { display: block; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.port-label-measure { display: inline-block; width: max-content; }
.hidden { opacity: 0; }
.short { position: absolute; inset: 0; }
</style>

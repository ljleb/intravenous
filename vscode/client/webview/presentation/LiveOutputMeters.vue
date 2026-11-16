<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useNode } from "./nodeContext";
import { useObservations } from "./observationContext";
import { addressKey, type NodeAddress } from "../../src/graphScope";

const node = useNode();
const observations = useObservations();
const root = ref<HTMLElement | null>(null);
let observer: IntersectionObserver | undefined;
const owner = Symbol("live output presentation");
let intersecting = false;
function updateVisibility(): void {
    if (current) observations?.node(owner, current, intersecting && !document.hidden);
}
let current: NodeAddress | undefined;
watch(() => node.value.address, (address) => {
    observations?.remove(owner);
    current = address;
    updateVisibility();
}, { immediate: true });
const items = computed(() => {
    observations?.version.value;
    const address = node.value.address;
    if (!address || !observations) return [];
    return observations.capabilities.value.filter((entry) => addressKey(entry.source.node) === addressKey(address))
        .map((entry) => ({ ...entry, result: observations.values.values.get(entry.id) }));
});
function magnitude(value: unknown): number {
    return typeof value === "number" && Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0;
}
function db(value: unknown): string {
    return typeof value === "number" && value > 0 ? `${(20 * Math.log10(value)).toFixed(1)} dB` : "−∞ dB";
}
onMounted(() => {
    document.addEventListener("visibilitychange", updateVisibility);
    observer = new IntersectionObserver(([entry]) => {
        intersecting = !!entry?.isIntersecting;
        updateVisibility();
    }, { rootMargin: "100px 0px" });
    if (root.value) observer.observe(root.value);
});
onBeforeUnmount(() => {
    document.removeEventListener("visibilitychange", updateVisibility);
    observer?.disconnect();
    observations?.remove(owner);
});
</script>

<template>
    <div ref="root" class="live-outputs" :title="observations?.error.value || undefined">
        <div v-for="item in items" :key="item.id" class="live-output" :class="{ unavailable: !item.result?.available }">
            <span class="output-name">{{ item.member }}</span>
            <template v-if="item.kind === 'output'">
                <div class="meter" role="meter" :aria-label="item.member" aria-valuemin="0" aria-valuemax="1"
                    :aria-valuenow="magnitude(item.result?.value?.peak)" :title="db(item.result?.value?.peak)">
                    <span class="meter-rms" :style="{ width: `${magnitude(item.result?.value?.rms) * 100}%` }" />
                    <span class="meter-peak" :style="{ left: `${magnitude(item.result?.value?.peak) * 100}%` }" />
                </div>
            </template>
            <output v-else>{{ item.result?.available ? item.result.value?.value : '—' }}</output>
        </div>
    </div>
</template>

<style scoped>
.live-outputs { height: 100%; display: flex; flex-direction: column; justify-content: center; gap: 4px; padding: 0 10px; box-sizing: border-box; }
.live-output { display: flex; align-items: center; gap: 8px; min-height: 14px; font-size: 11px; }
.output-name { flex: 0 1 90px; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; color: var(--vscode-descriptionForeground); }
.meter { height: 6px; position: relative; flex: 1; max-width: 300px; overflow: hidden; background: color-mix(in srgb, currentColor 10%, transparent); border-radius: 2px; }
.meter-rms { position: absolute; height: 100%; background: #579bd3; }
.meter-peak { position: absolute; height: 100%; width: 2px; background: #b9dcf7; }
.unavailable { opacity: .4; }
output { font-variant-numeric: tabular-nums; }
</style>

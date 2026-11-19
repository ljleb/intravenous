<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { MeterMode, ObservationDisplayValue } from "../../src/observations";
import { levelDomain, meterPosition, MeterPresentationState, valueDomain } from "../../src/meterPresentation";
import { numericTicks } from "../timeline/geometry";
import RulerGraduations from "../timeline/RulerGraduations.vue";
import { rightClickHint } from "../../src/statusBarText";

const props = defineProps<{ mode: MeterMode; label: string; result?: ObservationDisplayValue; active?: boolean;
    showScaleLabels: boolean }>();
const root = ref<HTMLElement | null>(null), width = ref(0);
const domain = computed(() => props.mode === "level" ? levelDomain : valueDomain);
const presentation = new MeterPresentationState();
const motionVersion = ref(0);
const updating = computed(() => { void motionVersion.value; return presentation.updating; });
const reading = computed(() => { void motionVersion.value; return presentation.reading(props.mode); });
const status = computed(() => { void motionVersion.value; return presentation.status(props.mode); });
let animation = 0;
function stopAnimation(): void { cancelAnimationFrame(animation); animation = 0; }
function animate(now: number): void {
    animation = 0;
    presentation.advance(now);
    ++motionVersion.value;
    if (presentation.moving) animation = requestAnimationFrame(animate);
}
watch([() => props.result, () => props.active], () => {
    stopAnimation();
    presentation.update(props.result, !!props.active, performance.now());
    ++motionVersion.value;
    if (presentation.moving) animation = requestAnimationFrame(animate);
}, { immediate: true });
const ticks = computed(() => {
    const { min, max } = domain.value;
    const ticks = numericTicks(min, max, width.value,
        (value) => props.mode === "level" ? `${value > 0 ? "+" : ""}${value}` : String(value), 60);
    // Explicit scale endpoints and unity must remain visible across widths.
    for (const value of [min, max, ...(props.mode === "level" ? [0] : [])]) {
        const x = meterPosition(value, min, max) * width.value;
        if (!ticks.some((tick) => Math.abs(tick.x - x) < 1)) ticks.push({ value, x,
            label: `${value > 0 && props.mode === "level" ? "+" : ""}${value}` });
    }
    return ticks.sort((a, b) => a.x - b.x).map((tick, index, all) => ({ ...tick,
        // Preserve grid lines while omitting crowded labels next to boundaries.
        label: index !== 0 && index !== all.length - 1
            && (tick.x < 30 || width.value - tick.x < 30) ? "" : tick.label }));
});
const ariaValue = computed(() => reading.value.available && !updating.value
    ? Math.max(domain.value.min, Math.min(domain.value.max, reading.value.displayValue!)) : undefined);
const meterHint = computed(() => `${props.label} · ${status.value}`);
const meterShortcuts = `${rightClickHint} scale`;
function focusLane(event: MouseEvent) {
    (event.currentTarget as HTMLElement).closest<HTMLElement>(".lane")?.focus({ preventScroll: true });
}
let resize: ResizeObserver | undefined;
onMounted(() => {
    resize = new ResizeObserver(([entry]) => { width.value = entry?.contentRect.width ?? 0; });
    if (root.value) resize.observe(root.value);
});
onBeforeUnmount(() => { stopAnimation(); resize?.disconnect(); });
</script>

<template>
    <div ref="root" class="sample-meter" :class="{ unavailable: !reading.available, 'over-reference': reading.overReference,
        'held-over-reference': reading.heldOverReference, 'hide-scale-labels': !showScaleLabels }"
        :data-mode="mode" :data-below="reading.available && reading.below" :data-above="reading.available && reading.above"
        role="meter" tabindex="-1" aria-haspopup="menu" :aria-label="label" :aria-valuemin="domain.min" :aria-valuemax="domain.max"
        :aria-valuenow="ariaValue" :aria-valuetext="status" :aria-busy="updating"
        :data-status-hint="meterHint" :data-status-shortcuts="meterShortcuts"
        @mousedown.left.prevent="focusLane" @mousedown.right.prevent="focusLane">
        <template v-if="reading.available">
            <span class="peak-fill" :style="{ width: `${reading.position * 100}%` }" />
            <span class="reading-mark" :style="{ left: `${reading.heldPosition * 100}%` }" />
        </template>
        <RulerGraduations :ticks="ticks" compact />
        <span v-if="!reading.available" class="unavailable-mark" aria-hidden="true">—</span>
    </div>
</template>

<style scoped>
.sample-meter { flex: 1; position: relative; min-width: 0; min-height: 0; height: auto; overflow: hidden; border: 1px solid color-mix(in srgb, var(--vscode-foreground) 18%, transparent); box-sizing: border-box; background: color-mix(in srgb, var(--vscode-foreground) 5%, transparent); }
.peak-fill { position: absolute; bottom: 0; left: 0; height: 35%; min-height: 2px; background: #3ab963; }
.reading-mark { position: absolute; bottom: 0; width: 2px; height: 45%; min-height: 3px; transform: translateX(-1px); background: #9ceaac; }
.over-reference .peak-fill, .held-over-reference .reading-mark { background: #f0b45c; }
.unavailable { background: repeating-linear-gradient(135deg, transparent, transparent 7px, color-mix(in srgb, var(--vscode-foreground) 4%, transparent) 7px, color-mix(in srgb, var(--vscode-foreground) 4%, transparent) 8px); }
.unavailable-mark { position: absolute; right: 5px; bottom: 1px; opacity: .6; }
.hide-scale-labels :deep(.ruler-tick span) { display: none; }
</style>

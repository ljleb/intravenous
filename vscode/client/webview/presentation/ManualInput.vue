<script setup lang="ts">
import { computed, onBeforeUnmount } from "vue";
import { useManualInput } from "./manualControlContext";
const props = defineProps<{ name: string; showName?: boolean }>();
const control = useManualInput(() => props.name);
const { value, available, limits, state, descriptor } = control;
const increment = computed(() => limits.value.min !== undefined && limits.value.max !== undefined
    ? (limits.value.max - limits.value.min) / 100 : Math.max(Math.abs(value.value ?? 0), 1) / 100);
onBeforeUnmount(() => control.hover(false));
const fraction = computed(() => {
    const { min, max, defaultValue } = limits.value;
    const center = defaultValue ?? 0;
    const current = value.value ?? center;
    return min !== undefined && max !== undefined && max > min
        ? Math.max(0, Math.min(1, (current - min) / (max - min)))
        : .5 + Math.atan((current - center) / Math.max(Math.abs(center), 1)) / Math.PI;
});
const indicator = computed(() => {
    const angle = (-135 + 270 * fraction.value) * Math.PI / 180;
    return { x: 16 + 9 * Math.sin(angle), y: 16 - 9 * Math.cos(angle) };
});
const logicalValue = computed(() => value.value === undefined ? "" : Number(value.value.toPrecision(6)).toString());
const normalizedValue = computed(() => limits.value.min !== undefined && limits.value.max !== undefined
    && limits.value.max > limits.value.min ? fraction.value.toFixed(3) : null);
function step(direction: number) {
    if (!available.value) return;
    control.edit(); control.update((value.value ?? 0) + direction * increment.value); control.finish();
}
</script>

<template>
    <div class="manual-input" :class="{ inactive: !descriptor?.participates, active: control.dragging.value }"
        :data-manual-control="control.key.value"
        @pointerdown.stop @click.stop @contextmenu="control.numeric"
        @pointerenter="control.hover(true)" @pointerleave="control.hover(false)">
        <span v-if="showName" class="manual-name">{{ name }}</span>
        <button type="button" class="manual-knob" role="slider" :aria-label="name"
            :aria-valuenow="value" :aria-valuemin="limits.min" :aria-valuemax="limits.max"
            :aria-disabled="!available" :disabled="!available" :title="`${name}: ${value}`"
            @focus="control.hover(true)" @blur="control.hover(false)"
            @pointerdown="control.scrub" @dblclick.stop.prevent @contextmenu="control.numeric"
            @keydown.left.stop.prevent="step(-1)" @keydown.down.stop.prevent="step(-1)"
            @keydown.right.stop.prevent="step(1)" @keydown.up.stop.prevent="step(1)">
            <svg viewBox="0 0 32 32" aria-hidden="true">
                <path class="knob-track" d="M 7.5 24.5 A 12 12 0 1 1 24.5 24.5" />
                <circle class="knob-body" cx="16" cy="16" r="9" />
                <line class="knob-indicator" x1="16" y1="16" :x2="indicator.x" :y2="indicator.y" />
            </svg>
        </button>
        <span class="manual-values" aria-hidden="true">
            <span class="logical-value">{{ logicalValue }}</span>
            <span v-if="normalizedValue !== null" class="normalized-value">{{ normalizedValue }}</span>
        </span>
        <span v-if="state?.error" class="manual-error" :title="state.error" aria-label="Manual input error">!</span>
    </div>
</template>

<style scoped>
.manual-input { display: flex; flex: 0 0 auto; align-items: center; gap: 4px; min-width: 0; padding: 0 4px; }
.manual-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.manual-knob { display: block; width: 30px; height: 30px; padding: 0; border: 0; border-radius: 50%;
    background: transparent; color: var(--vscode-terminal-ansiYellow); cursor: ns-resize; touch-action: none; user-select: none; }
.manual-knob svg { display: block; width: 100%; height: 100%; }
.manual-knob { flex: 0 0 30px; }
.manual-values { display: flex; flex-direction: column; min-width: 0; font-variant-numeric: tabular-nums; }
.logical-value { font-size: 12px; color: var(--vscode-foreground); overflow: hidden; text-overflow: ellipsis; }
.normalized-value { font-size: 10px; color: var(--vscode-descriptionForeground); }
.knob-track { fill: none; stroke: currentColor; stroke-width: 1.5; opacity: .3; }
.knob-body { fill: var(--vscode-editor-background); stroke: currentColor; stroke-width: 1; stroke-opacity: .55; }
.knob-indicator { stroke: currentColor; stroke-width: 2; stroke-linecap: round; opacity: .8; }
.knob-track, .knob-body, .knob-indicator { transition: fill 100ms ease, opacity 100ms ease, stroke-opacity 100ms ease; }
.manual-input:hover .manual-knob:not(:disabled) .knob-body { fill: color-mix(in srgb, currentColor 9%, var(--vscode-editor-background)); stroke-opacity: .9; }
.manual-input:hover .knob-track { opacity: .55; }
.manual-input:hover .knob-indicator { opacity: 1; }
.manual-input.active .manual-knob .knob-body { fill: color-mix(in srgb, currentColor 20%, var(--vscode-editor-background)); stroke-opacity: 1; }
.manual-input.active .knob-track { opacity: .9; }
.manual-input.active .knob-indicator { opacity: 1; }
.manual-knob:focus { outline: none; box-shadow: none; }
.manual-knob:focus-visible { box-shadow: 0 0 0 1px color-mix(in srgb, currentColor 45%, transparent); }
@media (prefers-reduced-motion: reduce) { .knob-track, .knob-body, .knob-indicator { transition: none; } }
.manual-knob:disabled { cursor: default; opacity: .4; }
.inactive .manual-name { color: var(--vscode-descriptionForeground); }
.manual-error { color: var(--vscode-errorForeground); }
</style>

<script setup lang="ts">
import { computed } from "vue";
import { knobFraction, knobMagnitude } from "../../src/knobPresentation";
import type { ManualControlLimits } from "../../src/manualControlGesture";

const props = defineProps<{ value: number; limits: ManualControlLimits }>();

function point(fraction: number, radius: number) {
    const angle = (-135 + 270 * fraction) * Math.PI / 180;
    return { x: 16 + radius * Math.sin(angle), y: 16 - radius * Math.cos(angle) };
}
const indicator = computed(() => point(knobFraction(props.value, props.limits), 9));
const defaultPin = computed(() => {
    const value = props.limits.defaultValue;
    if (value === undefined) return null;
    const fraction = knobFraction(value, props.limits);
    return { start: point(fraction, 13.6), end: point(fraction, 15.3) };
});
const magnitude = computed(() => knobMagnitude(props.value, props.limits));
</script>

<template>
    <svg viewBox="0 0 32 32" aria-hidden="true">
        <path class="knob-track" d="M 7.515 24.485 A 12 12 0 1 1 24.485 24.485" />
        <circle class="knob-body" cx="16" cy="16" r="9" />
        <circle v-if="limits.min !== undefined" class="bound" cx="7.515" cy="24.485" r="1.35" />
        <circle v-if="limits.max !== undefined" class="bound" cx="24.485" cy="24.485" r="1.35" />
        <line v-if="defaultPin" class="default-pin" :x1="defaultPin.start.x" :y1="defaultPin.start.y"
            :x2="defaultPin.end.x" :y2="defaultPin.end.y" />
        <line class="knob-indicator" x1="16" y1="16" :x2="indicator.x" :y2="indicator.y" />
        <g v-if="magnitude >= .1" class="magnitude">
            <rect v-for="i in 4" :key="i" :x="8.5 + (i - 1) * 4" y="27" width="3" height="2" rx=".5"
                :opacity=".16 + .84 * Math.min(1, Math.max(0, magnitude - i + 1))" />
        </g>
    </svg>
</template>

<style scoped>
.knob-track { fill: none; stroke: currentColor; stroke-width: 1.5; opacity: .34; }
.knob-body { fill: var(--vscode-editor-background); stroke: currentColor; stroke-width: 1; stroke-opacity: .55; }
.knob-indicator { stroke: currentColor; stroke-width: 2; stroke-linecap: round; opacity: .82; }
.bound, .magnitude { fill: currentColor; }
.default-pin { stroke: currentColor; stroke-width: 1.2; stroke-linecap: round; }
.knob-track, .knob-body, .knob-indicator { transition: fill 100ms ease, opacity 100ms ease, stroke-opacity 100ms ease; }
@media (prefers-reduced-motion: reduce) { .knob-track, .knob-body, .knob-indicator { transition: none; } }
</style>

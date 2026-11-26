<script setup lang="ts">
import { computed, useId } from "vue";
import { knobFraction, knobMagnitude } from "../../src/knobPresentation";
import type { ManualControlLimits } from "../../src/manualControlGesture";

const props = defineProps<{ value: number; limits: ManualControlLimits; portCutout?: boolean;
    accentGradient?: readonly [string, string] }>();
const gradientId = useId();

function point(fraction: number, radius: number) {
    const angle = (-135 + 270 * fraction) * Math.PI / 180;
    return { x: 16 + radius * Math.sin(angle), y: 16 - radius * Math.cos(angle) };
}
function arc(start: number, end: number) {
    const a = point(start, 12), b = point(end, 12);
    return `M ${a.x} ${a.y} A 12 12 0 ${end - start > 2 / 3 ? 1 : 0} 1 ${b.x} ${b.y}`;
}
const track = computed(() => {
    const left = props.limits.min === undefined;
    const right = props.limits.max === undefined;
    return [
        ...(left ? [arc(0, .024), arc(.047, .071)] : []),
        arc(left ? .094 : 0, right ? .906 : 1),
        ...(right ? [arc(.929, .953), arc(.976, 1)] : []),
    ].join(" ");
});
const boundPins = computed(() => [
    ...(props.limits.min === undefined ? [] : [0]),
    ...(props.limits.max === undefined ? [] : [1]),
].map((end) => ({ end, start: point(end, 10.4), finish: point(end, 14.8) })));
const fraction = computed(() => knobFraction(props.value, props.limits));
const defaultFraction = computed(() => {
    const value = props.limits.defaultValue;
    return value === undefined ? undefined : knobFraction(value, props.limits);
});
const indicator = computed(() => point(fraction.value, 9));
const defaultPin = computed(() => {
    const origin = defaultFraction.value;
    return origin === undefined ? null : { start: point(origin, 13.6), end: point(origin, 15.3) };
});
const valueSector = computed(() => {
    const origin = defaultFraction.value;
    if (origin === undefined || origin === fraction.value) return null;
    const radius = 8.5;
    const start = point(origin, radius);
    const end = point(fraction.value, radius);
    const delta = fraction.value - origin;
    const largeArc = Math.abs(delta) * 270 > 180 ? 1 : 0;
    const sweep = delta > 0 ? 1 : 0;
    return `M 16 16 L ${start.x} ${start.y} A ${radius} ${radius} 0 ${largeArc} ${sweep} ${end.x} ${end.y} Z`;
});
const magnitude = computed(() => knobMagnitude(props.value, props.limits));
</script>

<template>
    <svg viewBox="0 0 32 32" aria-hidden="true" :style="accentGradient ? { '--knob-paint': `url(#${gradientId})` } : undefined">
        <defs v-if="accentGradient">
            <linearGradient :id="gradientId" gradientUnits="userSpaceOnUse" x1="4" y1="16" x2="28" y2="16">
                <stop offset="0" :style="{ stopColor: accentGradient[0] }" />
                <stop offset="1" :style="{ stopColor: accentGradient[1] }" />
            </linearGradient>
        </defs>
        <path class="knob-track" :d="track" />
        <circle v-if="portCutout" class="knob-body knob-backing" cx="16" cy="16" r="11.25" />
        <circle class="knob-body" :class="{ 'compact-outline': portCutout }" cx="16" cy="16" r="9"
            :vector-effect="portCutout ? 'non-scaling-stroke' : undefined" />
        <path v-if="valueSector" class="knob-value-sector" :d="valueSector" />
        <line v-for="pin in boundPins" :key="pin.end" class="bound-pin" :x1="pin.start.x" :y1="pin.start.y"
            :x2="pin.finish.x" :y2="pin.finish.y" />
        <line v-if="defaultPin" class="default-pin" :x1="defaultPin.start.x" :y1="defaultPin.start.y"
            :x2="defaultPin.end.x" :y2="defaultPin.end.y" />
        <line class="knob-indicator" x1="16" y1="16" :x2="indicator.x" :y2="indicator.y" />
        <g v-if="magnitude > 0" class="magnitude">
            <rect v-for="i in 4" :key="i" :x="8.5 + (i - 1) * 4" y="27" width="3" height="2" rx=".5"
                :opacity=".16 + .84 * Math.min(1, Math.max(0, magnitude - i + 1))" />
        </g>
    </svg>
</template>

<style scoped>
.knob-track { fill: none; stroke: var(--knob-paint, currentColor); stroke-width: 1.5; opacity: .34; }
.knob-body { fill: var(--vscode-editor-background); stroke: var(--knob-paint, currentColor); stroke-width: 1; stroke-opacity: .55; }
.knob-body.knob-backing { stroke: none; }
.knob-body.compact-outline { stroke-opacity: .72; }
.knob-value-sector { fill: var(--knob-paint, currentColor); opacity: .17; }
.knob-indicator { stroke: var(--knob-paint, currentColor); stroke-width: 2; stroke-linecap: round; opacity: .82; }
.magnitude { fill: var(--knob-paint, currentColor); }
.bound-pin, .default-pin { stroke: var(--knob-paint, currentColor); stroke-width: 1.2; stroke-linecap: round; }
.knob-track, .knob-body, .knob-indicator { transition: fill 100ms ease, opacity 100ms ease, stroke-opacity 100ms ease; }
@media (prefers-reduced-motion: reduce) { .knob-track, .knob-body, .knob-indicator { transition: none; } }
</style>

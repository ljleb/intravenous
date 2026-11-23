<script setup lang="ts">
import { useId } from "vue";
import type { PresentationInk, PresentationMark, PresentationPoint } from "../../src/presentationPlugin";

defineProps<{ marks?: PresentationMark[]; highlightedTarget?: string }>();

function svgPoints(points: PresentationPoint[]) { return points.map(({ x, y }) => `${x},${y}`).join(" "); }
const cachedPackedPaths = new WeakMap<Float64Array, string>();
function packedSvgPoints(coordinates: Float64Array) {
    const existing = cachedPackedPaths.get(coordinates);
    if (existing !== undefined) return existing;
    let points = "";
    for (let i = 0; i < coordinates.length; i += 2) points += `${i ? " " : ""}${coordinates[i]},${coordinates[i + 1]}`;
    cachedPackedPaths.set(coordinates, points);
    return points;
}
const graphicsId = useId();
const inkColor: Record<PresentationInk, string> = { foreground: "--vscode-foreground", muted: "--vscode-descriptionForeground",
    input: "--iv-port-input", output: "--iv-port-output", selection: "--vscode-focusBorder", highlight: "--vscode-charts-blue",
    warning: "--vscode-charts-orange", background: "--vscode-editor-background" };
const color = (ink?: PresentationInk) => ink && `var(${inkColor[ink]})`;
</script>

<template>
    <svg class="plugin-scene" aria-hidden="true">
        <template v-for="(mark, i) in marks" :key="mark.id">
            <template v-if="mark.type === 'polyline' || mark.type === 'packedPolyline'">
                <defs v-if="mark.fade"><linearGradient :id="`${graphicsId}-fade-${i}`" gradientUnits="userSpaceOnUse"
                    :x1="mark.fade.from.x" :y1="mark.fade.from.y" :x2="mark.fade.to.x" :y2="mark.fade.to.y">
                    <stop offset="0" stop-opacity="0" :stop-color="color(mark.ink)" />
                    <stop offset=".25" stop-opacity=".2" :stop-color="color(mark.ink)" />
                    <stop offset=".5" stop-opacity=".95" :stop-color="color(mark.ink)" />
                    <stop offset=".75" stop-opacity=".2" :stop-color="color(mark.ink)" />
                    <stop offset="1" stop-opacity="0" :stop-color="color(mark.ink)" />
                </linearGradient></defs>
                <polyline :points="mark.type === 'polyline' ? svgPoints(mark.points) : packedSvgPoints(mark.coordinates)" :opacity="mark.opacity"
                    :class="{ 'plugin-hover-mark': mark.target && highlightedTarget === mark.target }"
                    :style="{ stroke: mark.fade ? `url(#${graphicsId}-fade-${i})` : (mark.fill ? 'none' : color(mark.ink)),
                        fill: mark.fill ? color(mark.ink) : undefined,
                        strokeWidth: mark.strokeWidth, strokeDasharray: mark.dashed ? '3 3' : undefined }" />
            </template>
            <rect v-else-if="mark.type === 'rect'" :x="mark.x" :y="mark.y" :width="mark.width" :height="mark.height"
                :opacity="mark.opacity" :style="{ fill: color(mark.ink), stroke: color(mark.border) }" />
            <circle v-else-if="mark.type === 'circle'" :cx="mark.x" :cy="mark.y" :r="mark.radius" :opacity="mark.opacity"
                :class="{ 'plugin-hover-mark': mark.target && highlightedTarget === mark.target }"
                :style="{ fill: color(mark.ink), stroke: color(mark.border), strokeWidth: 1.5 }" />
            <text v-else :x="mark.x" :y="mark.y" :opacity="mark.opacity" :style="{ fill: color(mark.ink) }">{{ mark.text }}</text>
        </template>
    </svg>
</template>

<style scoped>
.plugin-scene { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.plugin-scene polyline { fill: none; stroke: currentColor; stroke-width: 1; }
.plugin-scene text, .plugin-scene rect, .plugin-scene circle { fill: currentColor; }
.plugin-scene text { font: 10px monospace; }
.plugin-hover-mark { transform-box: fill-box; transform-origin: center; transform: scale(1.25); opacity: 1; }
</style>

<script setup lang="ts">
import type { RulerTick } from "./geometry";
defineProps<{ ticks: RulerTick[]; compact?: boolean; labelInset?: number; hideLabels?: boolean }>();
</script>

<template>
    <div class="ruler-graduations" :class="{ compact }" aria-hidden="true">
        <div v-for="tick in ticks" :key="tick.x" class="ruler-tick" :class="{ minor: tick.minor }"
            :style="{ left: `${tick.x}px`, '--label-offset': `${Math.max(0, (labelInset ?? 0) - tick.x)}px` }">
            <template v-if="!hideLabels">
                <span v-if="!tick.minor" :class="{ edge: compact && tick === ticks[ticks.length - 1] }">{{ tick.label }}</span>
                <span v-if="tick.sampleLabel" class="sample-label">{{ tick.sampleLabel }}</span>
            </template>
        </div>
    </div>
</template>

<style scoped>
.ruler-graduations { position: absolute; inset: 0; overflow: hidden; pointer-events: none; }
.ruler-tick { position: absolute; top: 0; bottom: 0; border-left: 1px solid var(--iv-ruler-tick); white-space: nowrap; font-variant-numeric: tabular-nums; }
.ruler-tick span { display: block; padding: 8px 5px; margin-left: var(--label-offset); }
.compact .ruler-tick { border-color: var(--iv-lane-ruler-tick); }
.compact .ruler-tick span.edge { transform: translateX(-100%); }
.compact .ruler-tick span { padding: 2px 3px; font-size: 9px; line-height: 11px; color: var(--vscode-descriptionForeground); }
.compact .ruler-tick .sample-label { padding-top: 0; }
.compact .ruler-tick.minor { border-color: color-mix(in srgb, var(--vscode-foreground) 9%, transparent); }
</style>

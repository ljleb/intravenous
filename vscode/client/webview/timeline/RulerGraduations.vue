<script setup lang="ts">
import type { RulerTick } from "./geometry";
defineProps<{ ticks: RulerTick[]; compact?: boolean }>();
</script>

<template>
    <div class="ruler-graduations" :class="{ compact }" aria-hidden="true">
        <div v-for="tick in ticks" :key="tick.x" class="ruler-tick" :style="{ left: `${tick.x}px` }">
            <span :class="{ edge: compact && tick === ticks[ticks.length - 1] }">{{ tick.label }}</span>
        </div>
    </div>
</template>

<style scoped>
.ruler-graduations { position: absolute; inset: 0; overflow: hidden; pointer-events: none; }
.ruler-tick { position: absolute; top: 0; bottom: 0; border-left: 1px solid var(--vscode-panel-border); white-space: nowrap; font-variant-numeric: tabular-nums; }
.ruler-tick span { display: block; padding: 8px 5px; }
.compact .ruler-tick { border-color: color-mix(in srgb, var(--vscode-foreground) 18%, transparent); }
.compact .ruler-tick span.edge { transform: translateX(-100%); }
.compact .ruler-tick span { padding: 2px 3px; font-size: 9px; line-height: 11px; color: var(--vscode-descriptionForeground); }
</style>

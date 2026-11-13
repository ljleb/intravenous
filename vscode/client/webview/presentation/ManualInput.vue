<script setup lang="ts">
import { computed, onBeforeUnmount } from "vue";
import { useManualInput } from "./manualControlContext";
const props = defineProps<{ name: string }>();
const control = useManualInput(() => props.name);
const { value, available, limits, state, descriptor } = control;
const increment = computed(() => limits.value.min !== undefined && limits.value.max !== undefined
    ? (limits.value.max - limits.value.min) / 100 : Math.max(Math.abs(value.value ?? 0), 1) / 100);
onBeforeUnmount(() => control.hover(false));
function input(event: Event) {
    const field = event.target as HTMLInputElement;
    if (field.value.length) control.update(field.valueAsNumber);
}
function blur(event: FocusEvent) {
    const target = event.target as HTMLInputElement;
    // Removing a widget is not the end of its host-owned gesture.
    queueMicrotask(() => {
        if (!target.isConnected) return;
        target.value = String(control.value.value ?? "");
        control.finish();
    });
}
</script>

<template>
    <label class="manual-input" :class="{ inactive: !descriptor?.participates }"
        @pointerenter="control.hover(true)" @pointerleave="control.hover(false)">
        <span class="manual-name">{{ name }}</span>
        <span class="manual-scrub" role="slider" tabindex="0" :aria-label="name"
            :aria-valuenow="value" :aria-valuemin="limits.min" :aria-valuemax="limits.max"
            :aria-disabled="!available" title="Drag to adjust" @pointerdown="control.scrub"
            @keydown.left.prevent="control.edit(); control.update((value ?? 0) - increment); control.finish()"
            @keydown.right.prevent="control.edit(); control.update((value ?? 0) + increment); control.finish()">↔</span>
        <input type="number" step="any" :aria-label="name" :value="value" :min="limits.min" :max="limits.max"
            :disabled="!available" @focus="control.edit()" @input="input" @blur="blur"
            @keydown.enter.stop.prevent="control.finish(); ($event.target as HTMLInputElement).blur()"
            @keydown.esc.stop.prevent="control.finish(); ($event.target as HTMLInputElement).blur()" />
        <span v-if="state?.error" class="manual-error" :title="state.error" aria-label="Manual input error">!</span>
    </label>
</template>

<style scoped>
.manual-input { display: flex; align-items: center; gap: 6px; min-width: 0; padding: 3px 10px; }
.manual-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.manual-scrub { cursor: ew-resize; color: var(--vscode-terminal-ansiYellow); touch-action: none; user-select: none; }
.manual-scrub[aria-disabled="true"] { cursor: default; opacity: .4; }
input { min-width: 60px; width: 85px; border: 1px solid var(--vscode-input-border, transparent); padding: 2px 4px;
    color: var(--vscode-input-foreground); background: var(--vscode-input-background); }
input:disabled { opacity: .5; }
.inactive .manual-name { color: var(--vscode-descriptionForeground); }
.manual-error { color: var(--vscode-errorForeground); }
</style>

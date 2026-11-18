<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from "vue";
import { useManualInput } from "./manualControlContext";
import { knobScrubSpeed } from "../../src/knobPresentation";
import { manualKnobStatus } from "../../src/manualControlGesture";
import KnobFace from "./KnobFace.vue";
const props = defineProps<{ name: string; showName?: boolean; direction?: "input" | "output";
    portCutout?: boolean }>();
const control = useManualInput(() => props.name, () => props.direction ?? "input");
const { value, available, limits, state, descriptor } = control;
const increment = computed(() => knobScrubSpeed(limits.value) * 2);
const knob = ref<HTMLButtonElement | null>(null);
const hovered = ref(false);
const preview = ref<{ left: string; top: string } | null>(null);
let previewTimer: ReturnType<typeof setTimeout> | undefined;
function positionPreview() {
    const bounds = knob.value?.getBoundingClientRect();
    if (!bounds) return;
    const center = bounds.left + bounds.width / 2;
    preview.value = { left: `${Math.max(46, Math.min(window.innerWidth - 46, center))}px`,
        top: `${Math.max(2, bounds.top - 82)}px` };
}
watch([hovered, control.dragging, available], ([over, dragging, enabled]) => {
    clearTimeout(previewTimer);
    if (!enabled || (!over && !dragging)) preview.value = null;
    else if (dragging || preview.value) positionPreview();
    else previewTimer = setTimeout(positionPreview, 300);
});
watch(() => preview.value !== null, (visible, _, onCleanup) => {
    if (!visible) return;
    window.addEventListener("resize", positionPreview);
    window.addEventListener("scroll", positionPreview, true);
    onCleanup(() => {
        window.removeEventListener("resize", positionPreview);
        window.removeEventListener("scroll", positionPreview, true);
    });
});
onBeforeUnmount(() => { clearTimeout(previewTimer); control.hover(false); });
const logicalValue = computed(() => value.value === undefined ? "" : Number(value.value.toPrecision(6)).toString());
const knobHint = computed(() => available.value && value.value !== undefined
    ? manualKnobStatus(props.name, value.value, limits.value.defaultValue !== undefined)
    : `${props.name} · Unavailable`);
function step(direction: number) {
    if (!available.value) return;
    control.edit(); control.update((value.value ?? 0) + direction * increment.value); control.finish();
}
</script>

<template>
    <div class="manual-input" :class="{ inactive: !descriptor?.participates, active: control.dragging.value }"
        :data-direction="direction ?? 'input'"
        :data-manual-control="control.key.value"
        :data-status-hint="knobHint"
        @pointerdown.stop @click.stop @contextmenu="control.numeric"
        @iv-hoverenter="control.hover(true)" @iv-hoverleave="control.hover(false)">
        <span v-if="showName" class="manual-name">{{ name }}</span>
        <button ref="knob" type="button" class="manual-knob" role="slider" :aria-label="name"
            :aria-valuenow="value" :aria-valuemin="limits.min" :aria-valuemax="limits.max"
            :aria-disabled="!available" :disabled="!available"
            @focus="control.hover(true)" @blur="control.hover(false)"
            @iv-hoverenter="hovered = true" @iv-hoverleave="hovered = false"
            @pointerdown="control.scrub" @dblclick.stop.prevent @contextmenu="control.numeric"
            @keydown.left.stop.prevent="step(-1)" @keydown.down.stop.prevent="step(-1)"
            @keydown.right.stop.prevent="step(1)" @keydown.up.stop.prevent="step(1)">
            <KnobFace :value="value ?? limits.defaultValue ?? 0" :limits="limits" :port-cutout="portCutout" />
        </button>
        <button v-if="direction === 'output'" type="button" class="output-invert"
            :class="{ inverted: control.inverted.value }" :aria-pressed="control.inverted.value"
            :disabled="!available" :aria-label="`${name}: invert output polarity`"
            :data-status-hint="control.inverted.value ? 'Restore polarity' : 'Invert polarity'"
            aria-description="Changes the sign and keeps the magnitude"
            @pointerdown.stop @click.stop="control.invert" @contextmenu.stop.prevent
            @focus="control.hover(true)" @blur="control.hover(false)">
            <svg viewBox="0 0 16 16" aria-hidden="true">
                <circle cx="8" cy="8" r="4.5" /><path d="M 3 13 L 13 3" />
            </svg>
        </button>
        <span v-if="state?.error" class="manual-error" :data-status-hint="state.error"
            role="img" :aria-label="`Manual control error. ${state.error}`">!</span>
        <Teleport to="body">
            <div v-if="preview" class="knob-preview" :data-direction="direction ?? 'input'" :style="preview" aria-hidden="true">
                <KnobFace :value="value ?? limits.defaultValue ?? 0" :limits="limits" />
                <span>{{ direction === 'output' && control.inverted.value ? '−' : '' }}{{ logicalValue }}</span>
            </div>
        </Teleport>
    </div>
</template>

<style scoped>
.manual-input { display: flex; flex: 0 0 auto; align-items: center; gap: 4px; min-width: 0; padding: 0 4px; }
.manual-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.manual-knob { display: block; width: var(--knob-size, 30px); height: var(--knob-size, 30px); padding: 0; border: 0; border-radius: 50%;
    background: transparent; color: var(--vscode-terminal-ansiYellow); cursor: ns-resize; touch-action: none; user-select: none; }
.manual-knob svg { display: block; width: 100%; height: 100%; }
.manual-input[data-direction="output"] .manual-knob, .knob-preview[data-direction="output"] { color: var(--vscode-terminal-ansiBlue); }
.output-invert { flex: 0 0 16px; width: 16px; height: 20px; padding: 1px; border: 0; border-radius: 3px;
    background: transparent; color: var(--vscode-descriptionForeground); cursor: pointer; }
.output-invert svg { display: block; width: 100%; height: 100%; fill: none; stroke: currentColor; stroke-width: 1.25; }
.output-invert:hover:not(:disabled), .output-invert.inverted { color: var(--vscode-terminal-ansiBlue);
    background: color-mix(in srgb, var(--vscode-terminal-ansiBlue) 12%, transparent); }
.output-invert:disabled { opacity: .4; cursor: default; }
.output-invert:focus-visible { outline: 1px solid var(--vscode-terminal-ansiBlue); outline-offset: 1px; }
.manual-knob { flex: 0 0 var(--knob-size, 30px); }
.manual-input[data-hovered] .manual-knob:not(:disabled) :deep(.knob-body) { fill: color-mix(in srgb, currentColor 9%, var(--vscode-editor-background)); stroke-opacity: .9; }
.manual-input[data-hovered] :deep(.knob-track) { opacity: .55; }
.manual-input[data-hovered] :deep(.knob-indicator) { opacity: 1; }
.manual-input.active .manual-knob :deep(.knob-body) { fill: color-mix(in srgb, currentColor 20%, var(--vscode-editor-background)); stroke-opacity: 1; }
.manual-input.active :deep(.knob-track) { opacity: .9; }
.manual-input.active :deep(.knob-indicator) { opacity: 1; }
.manual-knob:focus { outline: none; box-shadow: none; }
.manual-knob:focus-visible { box-shadow: 0 0 0 1px color-mix(in srgb, currentColor 45%, transparent); }
.manual-knob:disabled { cursor: default; opacity: .4; }
.inactive .manual-name { color: var(--vscode-descriptionForeground); }
.manual-error { color: var(--vscode-errorForeground); }
.knob-preview { position: fixed; z-index: 35; transform: translateX(-50%); pointer-events: none;
    display: grid; justify-items: center; gap: 2px; padding: 4px 8px;
    color: var(--vscode-terminal-ansiYellow); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 6px; }
.knob-preview svg { width: 48px; height: 48px; }
.knob-preview span { color: var(--vscode-foreground); font-variant-numeric: tabular-nums; font-size: 11px; }
</style>

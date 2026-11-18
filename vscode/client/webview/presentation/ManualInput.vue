<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from "vue";
import { useManualInput } from "./manualControlContext";
import { knobPortStatus, knobScrubSpeed } from "../../src/knobPresentation";
import { manualKnobStatus } from "../../src/manualControlGesture";
import KnobFace from "./KnobFace.vue";
const props = defineProps<{ name: string; showName?: boolean; direction?: "input" | "output";
    portCutout?: boolean }>();
const control = useManualInput(() => props.name, () => props.direction ?? "input");
const { value, available, limits, state, descriptor, activity } = control;
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
const knobHint = computed(() => {
    const port = knobPortStatus(control.port.value);
    if (!available.value || value.value === undefined)
        return { text: [props.name, port, "Unavailable"].filter(Boolean).join(" · ") };
    const hint = manualKnobStatus(props.name, value.value, limits.value.defaultValue !== undefined, port);
    if (activity.value === "retained") hint.text += " · Override held";
    else if (!descriptor.value?.enabled && activity.value !== "active") hint.text += " · Manual value off";
    return hint;
});
function step(direction: number) {
    if (!available.value) return;
    control.edit(); control.update((value.value ?? 0) + direction * increment.value); control.finish();
}
</script>

<template>
    <div class="manual-input" :class="{ inactive: !descriptor?.participates }"
        :data-direction="direction ?? 'input'"
        :data-activity="activity"
        :data-manual-control="control.key.value"
        :data-status-hint="knobHint.text" :data-status-shortcuts="knobHint.shortcuts"
        @pointerdown.stop @click.stop @contextmenu="control.numeric"
        @iv-hoverenter="control.hover(true)" @iv-hoverleave="control.hover(false)">
        <span v-if="showName" class="manual-name">{{ name }}</span>
        <button v-if="direction !== 'output'" type="button" class="knob-side-toggle knob-enable-toggle manual-input-toggle"
            :aria-pressed="!!descriptor?.enabled" :disabled="!descriptor?.available || descriptor.live || control.dragging.value"
            :aria-label="`${name} manual value`" :data-status-hint="descriptor?.enabled ? 'Disable manual value' : 'Enable manual value'"
            @pointerdown.stop @mousedown.left.prevent @click.stop="control.toggleEnabled" @contextmenu.stop.prevent>
            <span aria-hidden="true">{{ descriptor?.enabled ? '●' : '○' }}</span>
        </button>
        <button ref="knob" type="button" class="manual-knob" role="slider" :aria-label="name"
            :aria-valuenow="value" :aria-valuemin="limits.min" :aria-valuemax="limits.max"
            :aria-disabled="!available" :disabled="!available" :data-status-hint="knobHint.text" :data-status-shortcuts="knobHint.shortcuts"
            @focus="control.hover(true)" @blur="control.hover(false)"
            @iv-hoverenter="hovered = true" @iv-hoverleave="hovered = false"
            @pointerdown="control.scrub" @dblclick.stop.prevent @contextmenu="control.numeric"
            @keydown.left.stop.prevent="step(-1)" @keydown.down.stop.prevent="step(-1)"
            @keydown.right.stop.prevent="step(1)" @keydown.up.stop.prevent="step(1)">
            <KnobFace :value="value ?? limits.defaultValue ?? 0" :limits="limits" :port-cutout="portCutout" />
        </button>
        <button v-if="direction === 'output'" type="button" class="knob-side-toggle"
            :aria-pressed="control.inverted.value"
            :disabled="!available" :aria-label="`${name}: invert output polarity`"
            :data-status-hint="control.inverted.value ? 'Restore polarity' : 'Invert polarity'"
            aria-description="Changes the sign and keeps the magnitude"
            @pointerdown.stop @click.stop="control.invert" @contextmenu.stop.prevent
            @focus="control.hover(true)" @blur="control.hover(false)">
            <span aria-hidden="true">{{ control.inverted.value ? '−' : '+' }}</span>
        </button>
        <span v-if="state?.error" class="manual-error" :data-status-hint="state.error"
            role="img" :aria-label="`Manual control error. ${state.error}`">!</span>
        <Teleport to="body">
            <div v-if="preview" class="knob-preview" :class="{ inactive: !descriptor?.participates }"
                :data-direction="direction ?? 'input'" :data-activity="activity" :style="preview" aria-hidden="true">
                <KnobFace :value="value ?? limits.defaultValue ?? 0" :limits="limits" />
                <span>{{ direction === 'output' && control.inverted.value ? '−' : '' }}{{ logicalValue }}</span>
            </div>
        </Teleport>
    </div>
</template>

<style scoped>
.manual-input { display: flex; flex: 0 0 auto; align-items: center; gap: 4px; min-width: 0; padding: 0 4px; }
.manual-input[data-direction="output"] { position: relative; gap: 0; }
.manual-input-toggle { --knob-toggle-accent: var(--iv-sample-input); margin-inline-start: 0;
    margin-inline-end: calc(-1 * var(--output-polarity-inset, 2px)); }
.manual-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.manual-knob { display: block; width: var(--knob-size, 30px); height: var(--knob-size, 30px); padding: 0; border: 0; border-radius: 50%;
    background: transparent; color: var(--iv-sample-input); cursor: ns-resize; touch-action: none; user-select: none; }
.manual-knob svg { display: block; width: 100%; height: 100%; }
.manual-input[data-direction="output"] .manual-knob, .knob-preview[data-direction="output"] { color: var(--iv-sample-output); }
.manual-knob { flex: 0 0 var(--knob-size, 30px); }
.manual-input[data-hovered] .manual-knob:not(:disabled) :deep(.knob-body) { fill: color-mix(in srgb, currentColor 9%, var(--vscode-editor-background)); stroke-opacity: .9; }
.manual-input[data-hovered] :deep(.knob-track) { opacity: .55; }
.manual-input[data-hovered] :deep(.knob-indicator) { opacity: 1; }
.inactive:is([data-activity="idle"], [data-activity="hover"]) .manual-knob,
.knob-preview.inactive:is([data-activity="idle"], [data-activity="hover"]) svg { opacity: .42; }
.manual-input[data-activity="retained"] .manual-knob :deep(.knob-body),
.knob-preview[data-activity="retained"] :deep(.knob-body) {
    fill: color-mix(in srgb, currentColor 13%, var(--vscode-editor-background)); stroke-opacity: 1; }
[data-activity="retained"] :deep(.knob-track) { opacity: .72; }
.manual-input[data-activity="active"] .manual-knob :deep(.knob-body),
.knob-preview[data-activity="active"] :deep(.knob-body) {
    fill: color-mix(in srgb, currentColor 20%, var(--vscode-editor-background)); stroke-opacity: 1; }
[data-activity="active"] :deep(.knob-track) { opacity: .9; }
[data-activity="active"] :deep(.knob-indicator), [data-activity="retained"] :deep(.knob-indicator) { opacity: 1; }
.manual-knob:focus { outline: none; box-shadow: none; }
.manual-knob:focus-visible { box-shadow: 0 0 0 1px color-mix(in srgb, currentColor 45%, transparent); }
.manual-knob:disabled { cursor: default; opacity: .4; }
.inactive .manual-name { color: var(--vscode-descriptionForeground); }
.manual-error { color: var(--vscode-errorForeground); }
.manual-input[data-direction="output"] .manual-error { position: absolute; top: 0; left: 0; font-size: 11px; line-height: 1; }
.knob-preview { position: fixed; z-index: 35; transform: translateX(-50%); pointer-events: none;
    display: grid; justify-items: center; gap: 2px; padding: 4px 8px;
    color: var(--iv-sample-input); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 6px; }
.knob-preview svg { width: 48px; height: 48px; }
.knob-preview span { color: var(--vscode-foreground); font-variant-numeric: tabular-nums; font-size: 11px; }
</style>

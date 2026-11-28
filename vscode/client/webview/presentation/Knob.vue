<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef, watch } from "vue";
import { manualKnobStatus, type KnobActivity } from "../../src/manualControlGesture";
import { circularKnobRange, KnobScrub, normalizeKnobValue, type KnobLimits } from "../../src/knobScrub";
import { knobScrubSpeed } from "../../src/knobPresentation";
import { parseKnobValue, type KnobValueRange } from "../../src/knobValueEntry";
import { fitPopoverAtPoint } from "../../src/popoverPosition";
import { scrubSensitivityShortcut } from "../../src/scrubSensitivity";
import { useOptionalGraphEditor, type KnobDrag } from "../graph/context";
import KnobFace from "./KnobFace.vue";
import { isContextMenuKey } from "./contextPopover";
import { usePresentationSize } from "./presentationSize";

defineOptions({ inheritAttrs: false });
const props = defineProps<{
    identity: string; label: string; value?: number; limits: KnobLimits; faceLimits?: KnobLimits;
    valueUnit?: string;
    accentGradient?: readonly [string, string];
    disabled?: boolean; inactive?: boolean; inverted?: boolean; portCutout?: boolean; direction?: "input" | "output";
    activity?: KnobActivity; description?: string; step?: number; apply: (value: number, live: boolean) => void;
    reset?: () => void;
    beginEdit?: (event: PointerEvent) => boolean; endEdit?: (released: boolean) => void; editActive?: () => boolean;
    bindDrag?: (event: PointerEvent) => KnobDrag | undefined; bindValueEntry?: (anchor: DOMRect) => void;
    clipboard?: { value: number | undefined; range: KnobValueRange; apply: (value: number) => void };
}>();
const emit = defineEmits<{ hover: [hover: boolean] }>();
const editor = useOptionalGraphEditor();
const button = ref<HTMLButtonElement | null>(null);
const hovered = ref(false);
const dragging = computed(() => editor?.activeKnob.value === props.identity);
const activity = computed(() => dragging.value ? "active" : props.activity ?? (hovered.value ? "hover" : "idle"));
const value = computed(() => props.value ?? props.limits.defaultValue ?? 0);
function clipboardValue(): number | undefined {
    if (props.disabled) return undefined;
    const value = props.clipboard ? props.clipboard.value : props.value;
    return value !== undefined && Number.isFinite(value) ? value : undefined;
}
watch(button, (element, _, onCleanup) => {
    if (!element || !editor) return;
    onCleanup(editor.registerKnobClipboard(element, {
        identity: () => props.identity,
        value: clipboardValue,
        paste(text) {
            const current = clipboardValue();
            if (current === undefined) return "Knob value is unavailable.";
            const next = parseKnobValue(text, props.clipboard?.range ?? props.limits);
            if (typeof next === "string") return next;
            if (next === current) return;
            if (props.clipboard) props.clipboard.apply(next);
            else props.apply(next, false);
        },
    }));
});
const hint = computed(() => props.disabled || props.value === undefined
    ? { text: [props.label, props.description, "Unavailable"].filter(Boolean).join(" · "), shortcuts: undefined }
    : manualKnobStatus(props.label, value.value, props.limits.defaultValue !== undefined, props.description));
const previewTarget = shallowRef<HTMLElement | "body" | null>(null);
const previewPanel = shallowRef<HTMLElement | null>(null);
const previewSize = usePresentationSize(previewPanel);
const previewPosition = shallowRef<{ left: string; top: string }>();
let previewTimer: ReturnType<typeof setTimeout> | undefined;
function placePreview() {
    const anchor = button.value;
    const panel = previewPanel.value;
    if (!anchor || !panel || !previewTarget.value) return;
    const box = anchor.getBoundingClientRect();
    const size = { width: panel.offsetWidth, height: panel.offsetHeight };
    const above = box.top - size.height - 6;
    const point = fitPopoverAtPoint({ x: box.left + (box.width - size.width) / 2, y: above >= 4 ? above : box.bottom + 6 },
        size, { width: window.innerWidth, height: window.innerHeight });
    previewPosition.value = { left: `${point.x}px`, top: `${point.y}px` };
}
function showPreview() {
    const anchor = button.value;
    if (!anchor) return;
    const target = anchor.closest<HTMLElement>(":popover-open, [role=dialog]") ?? "body";
    if (previewTarget.value !== target) {
        previewPosition.value = undefined;
        previewTarget.value = target;
    } else placePreview();
}
watch([previewPanel, previewSize], placePreview, { flush: "post" });
watch([hovered, dragging, () => props.disabled], ([over, drag, unavailable]) => {
    clearTimeout(previewTimer);
    if (unavailable || (!over && !drag)) previewTarget.value = null;
    else if (drag || previewTarget.value) showPreview();
    else previewTimer = setTimeout(showPreview, 300);
});
watch(() => previewTarget.value !== null, (visible, _, onCleanup) => {
    if (!visible) return;
    window.addEventListener("resize", placePreview);
    window.addEventListener("scroll", placePreview, true);
    onCleanup(() => {
        window.removeEventListener("resize", placePreview);
        window.removeEventListener("scroll", placePreview, true);
    });
});
onBeforeUnmount(() => { clearTimeout(previewTimer); emit("hover", false); });

function beginDrag(event: PointerEvent): KnobDrag | undefined {
    if (props.bindDrag) return props.bindDrag(event);
    const { apply, endEdit, limits, label, editActive } = props;
    if (props.beginEdit && !props.beginEdit(event)) return;
    let current = value.value;
    const scrub = new KnobScrub(knobScrubSpeed(limits), -event.clientY, current, limits, event.ctrlKey && !limits.wholeByDefault);
    function move(position: number) {
        scrub.refresh(props.limits, current);
        const next = scrub.move(position, current);
        if (current === next) return;
        current = next;
        apply(next, true);
    }
    return { move: ({ y }) => move(-y), moveBy: ({ y }) => move(scrub.position - y),
        changeSensitivity: (delta) => scrub.changeSensitivity(delta, current), finish: (point) => endEdit?.(point !== null), active: editActive,
        hint: () => ({ text: `${label} ${Number(current.toPrecision(6))} · ${scrub.hint()}`,
            shortcuts: `${scrubSensitivityShortcut} · Esc release` }) };
}
function set(value: number) {
    const next = normalizeKnobValue(value, props.limits);
    if (!props.disabled && next !== props.value) props.apply(next, false);
}
function press(event: PointerEvent) {
    if (event.button !== 0 || !event.isPrimary || !editor || props.disabled) return;
    event.preventDefault(); event.stopPropagation();
    const reset = props.limits.defaultValue;
    editor.startKnob(event, props.identity, () => beginDrag(event), reset === undefined ? undefined : props.reset ?? (() => set(reset)));
}
function entry(event: Event) {
    event.preventDefault(); event.stopPropagation();
    if (!button.value || props.disabled) return;
    const apply = props.apply;
    editor?.openKnobValue(button.value, props.label, value.value, () => props.limits, (value) => apply(value, false), props.bindValueEntry);
}
function step(direction: number) {
    const delta = props.step ?? (props.limits.wholeByDefault || props.limits.integer ? 1 : knobScrubSpeed(props.limits) * 2);
    if (circularKnobRange(props.limits)) {
        const current = props.limits.wholeByDefault || props.limits.integer ? Math.round(value.value) : value.value;
        set(current + direction * delta);
        return;
    }
    if (props.limits.wholeByDefault || props.limits.integer) {
        const min = Math.ceil(props.limits.min ?? -Infinity), max = Math.floor(props.limits.max ?? Infinity);
        if (min <= max) set(Math.max(min, Math.min(max, Math.round(value.value) + direction * delta)));
    } else set(value.value + direction * delta);
}
function keyboard(event: KeyboardEvent) {
    if (isContextMenuKey(event)) entry(event);
    if (event.key === "Escape" && editor?.cancelKnob()) { event.preventDefault(); event.stopPropagation(); }
}
defineExpose({ entry });
</script>

<template>
    <button v-bind="$attrs" ref="button" type="button" tabindex="-1" class="knob-control" role="slider"
        :aria-label="label" :aria-valuenow="value" :aria-valuetext="valueUnit ? `${value} ${valueUnit}` : undefined"
        :aria-valuemin="limits.min" :aria-valuemax="limits.max" :disabled="disabled" :data-activity="activity"
        :data-direction="direction ?? 'input'" :class="{ inactive }" :data-status-hint="hint.text" :data-status-shortcuts="hint.shortcuts"
        @iv-hoverenter="hovered = true" @iv-hoverleave="hovered = false" @focus="emit('hover', true)"
        @blur="emit('hover', false)"
        @pointerdown="press" @click.stop @contextmenu="entry" @dblclick.stop.prevent @keydown="keyboard"
        @keydown.left.stop.prevent="step(-1)" @keydown.down.stop.prevent="step(-1)"
        @keydown.right.stop.prevent="step(1)" @keydown.up.stop.prevent="step(1)"
        @keydown.home.stop.prevent="limits.min !== undefined && set(limits.min)"
        @keydown.end.stop.prevent="limits.max !== undefined && set(limits.max)">
        <KnobFace :value="value" :limits="faceLimits ?? limits" :port-cutout="portCutout" :accent-gradient="accentGradient" />
    </button>
    <Teleport v-if="previewTarget" :to="previewTarget">
        <div ref="previewPanel" class="knob-preview" :class="{ inactive }" :data-direction="direction ?? 'input'" :data-activity="activity"
            :style="previewPosition ?? { visibility: 'hidden' }" aria-hidden="true">
            <span class="knob-preview-title">{{ label }}</span>
            <KnobFace :value="value" :limits="faceLimits ?? limits" :accent-gradient="accentGradient" />
            <span class="knob-preview-value">{{ inverted ? '−' : '' }}{{ Number(value.toPrecision(6)) }}{{ valueUnit ? ` ${valueUnit}` : '' }}</span>
        </div>
    </Teleport>
</template>

<style scoped>
.knob-control { display: block; flex: 0 0 var(--knob-size, 30px); width: var(--knob-size, 30px); height: var(--knob-size, 30px);
    padding: 0; border: 0; border-radius: 50%; background: transparent; color: var(--iv-port-input);
    cursor: ns-resize; touch-action: none; user-select: none; }
.knob-control svg { display: block; width: 100%; height: 100%; }
.knob-control[data-direction="output"], .knob-preview[data-direction="output"] { color: var(--iv-port-output); }
.knob-control[data-activity="hover"]:not(:disabled) :deep(.knob-body) { fill: color-mix(in srgb, currentColor 9%, var(--vscode-editor-background)); stroke-opacity: .9; }
.knob-control[data-activity="hover"] :deep(.knob-track) { opacity: .55; }
.knob-control[data-activity="hover"] :deep(.knob-indicator) { opacity: 1; }
.knob-control.inactive:is([data-activity="idle"], [data-activity="hover"]),
.knob-preview.inactive:is([data-activity="idle"], [data-activity="hover"]) svg { opacity: .42; }
[data-activity="retained"] :deep(.knob-body), [data-activity="active"] :deep(.knob-body) {
    fill: color-mix(in srgb, currentColor 20%, var(--vscode-editor-background)); stroke-opacity: 1; }
[data-activity="active"] :deep(.knob-track) { opacity: .9; }
[data-activity="active"] :deep(.knob-indicator), [data-activity="retained"] :deep(.knob-indicator) { opacity: 1; }
.knob-control:disabled { cursor: default; opacity: .4; }
.knob-preview { position: fixed; z-index: 35; pointer-events: none; box-sizing: border-box;
    width: max-content; max-width: min(180px, calc(100vw - 8px));
    display: grid; justify-items: center; gap: 2px; padding: 4px 8px; color: var(--iv-port-input);
    background: var(--vscode-editorWidget-background); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); border-radius: 6px; }
.knob-preview svg { width: 48px; height: 48px; }
.knob-preview span { color: var(--vscode-foreground); font-size: 11px; line-height: 14px; }
.knob-preview-title { min-width: 0; text-align: center; overflow-wrap: anywhere; overflow: hidden;
    display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; }
.knob-preview-value { font-variant-numeric: tabular-nums; }
</style>

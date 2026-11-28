<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from "vue";
import { ConnectionDelayEdits, type GraphConnectionEnd } from "../../src/graphConnectionEditing";
import type { KnobLimits } from "../../src/knobScrub";
import { useGraphEditor } from "../graph/context";
import Knob from "./Knob.vue";

const props = defineProps<{ end: GraphConnectionEnd; identity: string; portName: string; portCutout?: boolean }>();
const editor = useGraphEditor();
const limits: KnobLimits = { min: 1, defaultValue: 1, integer: true, scrubSpeed: 1 };
const inputGradient = ["var(--iv-feedback-delay-color)", "var(--iv-port-input)"] as const;
const outputGradient = ["var(--iv-port-output)", "var(--iv-feedback-delay-color)"] as const;
const stored = computed(() => props.end.connection.connection.loopExtraLatency);
const draft = ref<number>();
const dragging = ref(false);
const disabled = computed(() => stored.value === undefined || !editor.scope.value?.editable || editor.historyPending.value);
const edits = new ConnectionDelayEdits(editor.setConnectionDelay, (pending) => {
    if (!pending && !dragging.value) draft.value = undefined;
}, () => {
    if (editor.activeKnob.value === props.identity) editor.cancelKnob();
});

function begin(): boolean {
    if (disabled.value || !edits.begin(props.end)) return false;
    dragging.value = true;
    return true;
}

function apply(samples: number, live: boolean) {
    if (disabled.value || (live && !dragging.value)) return;
    if (!live && !edits.pending && !edits.begin(props.end)) return;
    draft.value = samples;
    edits.update(samples);
}

function finish(released: boolean) {
    dragging.value = false;
    if (!released) edits.discard();
    if (!released || !edits.pending) draft.value = undefined;
}

watch(disabled, (unavailable) => {
    if (!unavailable) return;
    if (editor.activeKnob.value === props.identity) editor.cancelKnob();
    edits.discard();
    draft.value = undefined;
}, { flush: "sync" });
onBeforeUnmount(() => {
    if (dragging.value && editor.activeKnob.value === props.identity) editor.cancelKnob();
});
</script>

<template>
    <Knob :identity="identity" :label="`${portName} | Feedback delay`" value-unit="smp"
        :value="draft ?? stored" :limits="limits"
        :accent-gradient="end.port.direction === 'input' ? inputGradient : outputGradient"
        :direction="end.port.direction" :port-cutout="portCutout"
        :disabled="disabled" :apply="apply" :begin-edit="begin" :end-edit="finish" />
</template>

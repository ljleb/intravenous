<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef, watch } from "vue";
import { beatBpm, readBeatGrid, type BeatGrid } from "../../src/beatGrid";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { presentationErrorDetails } from "../../src/presentationDiagnostics";
import type { PresentationFrame, PresentationInput, PresentationSnapshot } from "../../src/presentationPlugin";
import { PresentationPluginSession } from "../../src/presentationPluginSession";
import { ReloadableWasmPresentation } from "../../src/reloadableWasmPresentation";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineWindow } from "../timeline/timelineContext";
import { useBeatGridEditing } from "./beatGridEditingContext";
import PresentationControls from "./PresentationControls.vue";
import { usePresentationFrame } from "./presentationFrames";
import { createWebPresentationWorker } from "./presentationPluginWorker";
import { shippedPresentation } from "./presentations";

const props = defineProps<{ target: NodeAddress; grid: BeatGrid; open: boolean }>();
const emit = defineEmits<{ done: [] }>();
const time = useTimelineWindow();
const editor = useOptionalGraphEditor();
const sampleRate = computed(() => time?.value.sampleRate);
const { edits, version } = useBeatGridEditing();
const program = computed(() => shippedPresentation("builtin/beats"));
const panel = shallowRef<PresentationFrame["panel"]>();
const failure = ref("");
const error = computed(() => { void version.value; return failure.value || edits.error(props.target); });
function report(operation: "frame" | "reload", reason: unknown) {
    failure.value = String(reason).slice(0, 512);
    if (!editor) return;
    editor.reportPresentationError(presentationErrorDetails({ surface: "beats-settings", operation,
        address: props.target, definitionId: "builtin.beats", artifact: program.value }, reason));
}
const worker = new ReloadableWasmPresentation(createWebPresentationWorker, (reason) => {
    if (reason === undefined) failure.value = "";
    else report("reload", reason);
});
const job = usePresentationFrame("update", () => { void session.flush(); });
const session = new PresentationPluginSession((snapshot, inputs) => {
    if (!program.value) throw new Error("Beats settings are unavailable");
    return worker.run(program.value.bytes, snapshot, inputs, () => true);
}, job.schedule, (frame) => {
    panel.value = frame.panel;
    for (const command of frame.commands ?? []) {
        if (command.type === "finish") {
            void edits.finish(props.target);
            continue;
        }
        // The ruler's internal preview uses the same pending grid as its
        // observation overlay and retains the existing shared edit transport.
        const payload = command.event.payload;
        if (!payload || Array.isArray(payload) || typeof payload !== "object") continue;
        const grid = readBeatGrid([payload.bpm, payload.beats_per_bar, payload.subdivisions]);
        if (grid) edits.update(props.target, grid, !!command.gesture);
    }
}, (reason) => { report("frame", reason); });
const snapshot = computed<PresentationSnapshot | undefined>(() => {
    void version.value;
    if (!props.open || !program.value) return;
    const draft = edits.draft(props.target);
    const grid = draft ?? props.grid;
    return { node: { address: props.target, definitionId: "builtin.beats",
        arguments: [beatBpm(grid), grid.beatsPerBar, grid.subdivisions], ports: [] },
        revision: 0, mode: "editable", editable: true, editsPending: !!draft, editError: error.value,
        viewport: { width: 1, height: 1, time: sampleRate.value === undefined ? undefined :
            { beginSample: "0", beginFraction: 0, spanSamples: 1, sampleRate: sampleRate.value } },
        observations: {}, state: null };
});
watch([snapshot, program], ([value]) => { if (value) session.update(value); }, { immediate: true });
const unregisterInterrupt = editor?.registerPresentationInterrupt((reason) => {
    if (program.value && (props.open || session.pending)) session.interrupt(reason);
});

function change(input: Extract<PresentationInput, { type: "control" }>) { session.input(input); }
async function finish() {
    await session.whenIdle();
    await edits.finish(props.target);
}
async function done() {
    await finish();
    emit("done");
}
defineExpose({ finish });
onBeforeUnmount(() => { unregisterInterrupt?.(); session.dispose(); worker.dispose(); });
</script>

<template>
    <PresentationControls v-if="panel" :rows="panel.rows" :identity="`${addressKey(target)}:beats`" editable @input="change" />
    <button class="beat-settings-done" type="button" tabindex="-1" @click="done">Done</button>
    <span v-if="error" role="alert" class="beat-settings-error">{{ error }}</span>
</template>

<style scoped>
.beat-settings-done { align-self: end; border: 0; border-radius: 3px; padding: 3px 6px; font: inherit;
    background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground); }
.beat-settings-error { color: var(--vscode-errorForeground); }
</style>

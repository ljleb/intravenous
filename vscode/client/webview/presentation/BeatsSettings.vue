<script setup lang="ts">
import { computed } from "vue";
import type { BeatGrid } from "../../src/beatGrid";
import { beatSettingsRows, editBeatSetting } from "../../src/beatsPresentationPlugin";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import type { PresentationInput } from "../../src/presentationPlugin";
import { useTimelineWindow } from "../timeline/timelineContext";
import { useBeatGridEditing } from "./beatGridEditingContext";
import PresentationControls from "./PresentationControls.vue";

const props = defineProps<{ target: NodeAddress; grid: BeatGrid }>();
const emit = defineEmits<{ done: [] }>();
const time = useTimelineWindow();
const { edits, version } = useBeatGridEditing();
const preview = computed(() => { void version.value; return edits.view(props.target) ?? props.grid; });
const error = computed(() => { void version.value; return edits.error(props.target); });
const rows = computed(() => beatSettingsRows(preview.value, time?.value.sampleRate ?? undefined));

function change(input: Extract<PresentationInput, { type: "control" }>) {
    if (input.phase === "finish") { void edits.finish(props.target); return; }
    const next = editBeatSetting(preview.value, input, time?.value.sampleRate ?? undefined);
    if (next) edits.update(props.target, next, input.phase === "preview");
}
</script>

<template>
    <PresentationControls :rows="rows" :identity="`${addressKey(target)}:beats`" editable @input="change" />
    <button class="beat-settings-done" type="button" tabindex="-1" @click="emit('done')">Done</button>
    <span v-if="error" role="alert" class="beat-settings-error">{{ error }}</span>
</template>

<style scoped>
.beat-settings-done { align-self: end; border: 0; border-radius: 3px; padding: 3px 6px; font: inherit;
    background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground); }
.beat-settings-error { color: var(--vscode-errorForeground); }
</style>

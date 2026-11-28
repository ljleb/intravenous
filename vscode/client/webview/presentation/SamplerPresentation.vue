<script setup lang="ts">
import { computed, ref } from "vue";
import { isProjectInstance } from "../../src/graphScope";
import { useOptionalGraphEditor } from "../graph/context";
import type { NodeDisplayMode } from "./displayMode";
import LaneSettings from "./LaneSettings.vue";
import LiveOutputMeters from "./LiveOutputMeters.vue";
import { useNode } from "./nodeContext";
import NodeLane from "./NodeLane.vue";

defineProps<{ mode: NodeDisplayMode }>();
const node = useNode();
const editor = useOptionalGraphEditor();
const directoryPending = ref(false);
const editable = computed(() => !!editor?.scope.value?.editable && !editor.historyPending.value);

const directory = ref("");
const initialDirectory = ref("");
const previousSource = ref("");
const directoryError = computed(() => {
    const value = directory.value;
    return !value.trim() || value.startsWith("/") || value.split("/").includes("..")
        || /[\u0000-\u001f\ud800-\udfff]/u.test(value)
        ? "Enter a directory inside the project." : "";
});

function startDirectoryEdit(open: boolean) {
    if (!open || directoryPending.value) return;
    const { configurationSource, construction } = node.value;
    previousSource.value = configurationSource ?? "";
    if (!previousSource.value.trim()) {
        directory.value = initialDirectory.value = "recordings";
        return;
    }
    const argument = construction?.arguments[0];
    directory.value = typeof argument === "string" ? argument : "recordings";
    // The durable configuration can arrive before the newly constructed node.
    try {
        const desired: unknown = JSON.parse(previousSource.value);
        if (typeof desired === "string") directory.value = desired;
    } catch {
        // The accepted arguments also cover non-JSON configuration expressions.
    }
    initialDirectory.value = directory.value;
}

async function saveDirectory(close: () => void) {
    const address = node.value.address;
    if (!editor || !address || !isProjectInstance(address) || !editable.value
        || directoryPending.value || directoryError.value) return;
    directoryPending.value = true;
    try {
        const saved = await editor.setNodeConfiguration(address.instanceId, node.value.displayName ?? "",
            previousSource.value, JSON.stringify(directory.value));
        if (saved) close();
    } finally {
        directoryPending.value = false;
    }
}
</script>

<template>
    <NodeLane :mode="mode" flush-content>
        <div class="sampler-content">
            <LiveOutputMeters :mode="mode" />
            <LaneSettings v-if="mode === 'editable' && editor && node.address && isProjectInstance(node.address)"
                label="Settings" dialog-label="Sampler settings" hint="Set the recording directory"
                :width="264" :preferred-height="117" @toggle="startDirectoryEdit">
                <template #default="{ close }">
                    <form class="sampler-settings" @submit.prevent="saveDirectory(close)">
                        <label>Recording directory
                            <input v-model="directory" placeholder="recordings" spellcheck="false"
                                :disabled="directoryPending" :aria-invalid="!!directoryError" />
                        </label>
                        <span v-if="directoryError" class="directory-error" role="status">{{ directoryError }}</span>
                        <button type="submit" :disabled="!editable || !!directoryError || directoryPending
                            || directory === initialDirectory">Apply</button>
                    </form>
                </template>
            </LaneSettings>
        </div>
    </NodeLane>
</template>

<style scoped>
.sampler-content { position: relative; height: 100%; }
.sampler-settings { display: grid; gap: 6px; padding: 8px; }
.sampler-settings label { display: grid; gap: 4px; min-width: 0; }
.sampler-settings input {
    min-width: 0; padding: 4px; font: inherit;
    color: var(--vscode-input-foreground); background: var(--vscode-input-background);
    border: 1px solid var(--vscode-input-border, var(--vscode-panel-border));
}
.sampler-settings button {
    justify-self: end; padding: 3px 8px; border: 0;
    color: var(--vscode-button-foreground); background: var(--vscode-button-background);
}
.sampler-settings button:disabled { opacity: .55; }
.directory-error { color: var(--vscode-errorForeground); }
</style>

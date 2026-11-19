<script setup lang="ts">
import { ref, watch } from "vue";
import type { GraphScopeNode } from "../../src/graphScope";
import { webviewApi } from "../host";
import { presentations } from "../presentation/presentations";

const props = defineProps<{ node?: GraphScopeNode; definitions: string[] }>();
const emit = defineEmits<{ close: [] }>();
const instance = props.node?.instance;
const definitionId = ref(instance?.definitionId ?? props.definitions[0] ?? "");
const previousDisplayName = instance?.displayName ?? "";
const previousConfigurationSource = instance?.configurationSource ?? "";
const displayName = ref(previousDisplayName);
const configurationSource = ref(previousConfigurationSource);
// A new node starts from its presentation's construction data, until the user
// types their own arguments.
const initialConfiguration = (id: string) =>
    presentations.candidates(id).find((entry) => entry.initialConfiguration !== undefined)?.initialConfiguration ?? "";
let suggested = "";
watch(definitionId, (id) => {
    if (instance || configurationSource.value !== suggested) return;
    suggested = initialConfiguration(id);
    configurationSource.value = suggested;
}, { immediate: true });
function save() {
    if (instance) {
        webviewApi.postMessage({ type: "updateGraphNode", instanceId: instance.instanceId,
            displayName: displayName.value, configurationSource: configurationSource.value,
            previousDisplayName, previousConfigurationSource });
    } else {
        webviewApi.postMessage({ type: "createGraphNode", definitionId: definitionId.value,
            displayName: displayName.value, configurationSource: configurationSource.value });
    }
    emit("close");
}
</script>

<template>
    <Teleport to="body">
        <div class="node-editor-backdrop" @click.self="emit('close')" @keydown.esc.stop="emit('close')">
            <form class="node-editor" role="dialog" aria-modal="true" aria-label="Node configuration" @submit.prevent="save">
                <label>Definition
                    <input v-if="instance" :value="definitionId" tabindex="-1" readonly />
                    <select v-else v-model="definitionId" tabindex="-1" autofocus>
                        <option v-for="id in definitions" :key="id" :value="id">{{ id }}</option>
                    </select>
                </label>
                <label>Name<input v-model="displayName" tabindex="-1" /></label>
                <label>Construction arguments<textarea v-model="configurationSource" tabindex="-1" rows="6" spellcheck="false" /></label>
                <footer>
                    <button type="button" tabindex="-1" @click="emit('close')">Cancel</button>
                    <button type="submit" tabindex="-1" :disabled="!definitionId">{{ instance ? 'Apply' : 'Create' }}</button>
                </footer>
            </form>
        </div>
    </Teleport>
</template>

<style scoped>
.node-editor-backdrop { position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; background: #0005; }
.node-editor { width: min(520px, calc(100vw - 32px)); padding: 16px; box-sizing: border-box; display: grid; gap: 12px; border: 1px solid var(--vscode-panel-border); background: var(--vscode-editorWidget-background); color: var(--vscode-foreground); }
label { display: grid; gap: 4px; }
input, select, textarea { min-width: 0; box-sizing: border-box; width: 100%; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, transparent); padding: 5px; }
textarea { resize: vertical; font-family: var(--vscode-editor-font-family); }
footer { display: flex; gap: 8px; justify-content: flex-end; }
button { padding: 4px 10px; cursor: pointer; background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: 0; }
</style>

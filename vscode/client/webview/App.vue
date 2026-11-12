<script setup lang="ts">
import { computed, ref, watch } from "vue";

import { hostState, loadLocalState, postToHost, saveLocalState } from "./host";
import { instanceLabel, visibleNodes } from "./liveGraphView";
import { isNodeViewChoice, nodeViewChoices, type NodeViewChoice } from "./presentation/displayMode";
import PresentationHost from "./presentation/PresentationHost.vue";

const storedViewChoice = loadLocalState().viewChoice;
const viewChoice = ref<NodeViewChoice>(
    isNodeViewChoice(storedViewChoice) ? storedViewChoice : "mini");
watch(viewChoice, (choice) => saveLocalState({ ...loadLocalState(), viewChoice: choice }));

const state = computed(() => hostState.value);
const nodes = computed(() => visibleNodes(state.value.nodes, state.value.selectedInstanceId));

// The host answers with the resolved selection; until then the select keeps
// showing the user's choice.
const selectedInstanceId = computed({
    get: () => state.value.selectedInstanceId ?? "",
    set: (instanceId: string) => postToHost({
        type: "selectInstance",
        instanceId: instanceId.length > 0 ? instanceId : null,
    }),
});

const statusText = computed(() => {
    switch (state.value.sessionStatus) {
    case "running":
        return "";
    case "restarting":
        return "The Intravenous server stopped; restarting it…";
    case "starting":
        return "Starting the Intravenous server…";
    }
});

function createInstance(): void {
    postToHost({ type: "createInstance" });
}
</script>

<template>
    <div class="live-graph">
        <div class="toolbar">
            <label class="toolbar-label" for="instance-select">instance</label>
            <select
                id="instance-select"
                v-model="selectedInstanceId"
                class="toolbar-select"
                aria-label="Selected iv module instance"
                :disabled="state.instances.length === 0"
            >
                <option value="">
                    {{ state.instances.length > 0 ? "[all available instances]" : "[no instances]" }}
                </option>
                <option
                    v-for="instance in state.instances"
                    :key="instance.instanceId"
                    :value="instance.instanceId"
                >
                    {{ instanceLabel(instance) }}
                </option>
            </select>
            <button
                class="toolbar-button"
                type="button"
                :disabled="!state.packageRoot"
                @click="createInstance"
            >
                Duplicate
            </button>
        </div>
        <div class="toolbar">
            <label class="toolbar-label" for="view-select">view</label>
            <select id="view-select" v-model="viewChoice" class="toolbar-select">
                <option v-for="choice in nodeViewChoices" :key="choice" :value="choice">
                    {{ choice }}
                </option>
            </select>
        </div>
        <div v-if="statusText" class="status">{{ statusText }}</div>
        <div
            v-if="nodes.length === 0"
            class="empty"
            title="No visible virtual nodes at the current selection"
        >
            [no nodes]
        </div>
        <PresentationHost
            v-for="(node, position) in nodes"
            :key="node.id ?? `position:${position}`"
            :node="node"
            :view-choice="viewChoice"
        />
    </div>
</template>

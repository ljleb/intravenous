<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, shallowRef } from "vue";
import type { GraphEditorHostMessage, GraphEditorState } from "../../src/graphEditorProtocol";
import { placeNodes, readLayout } from "../../src/graphScope";
import { webviewApi } from "../host";
import PresentationHost from "../presentation/PresentationHost.vue";
import type { SemanticNode } from "../presentation/semanticNode";
import { provideGraphEditor } from "./context";

const state = shallowRef<GraphEditorState>({ sessionStatus: "starting", scope: null, layout: readLayout(null), error: "" });
const layout = ref(readLayout(null));
let initializedLayout = false;
const root = computed<SemanticNode>(() => ({
    id: "", instanceId: "", definitionId: "", kind: "Project graph", sourceIdentity: "",
    memberCount: 0, ports: [], members: [], sourceSpans: [], scope: state.value.scope ?? undefined,
}));
function save() { webviewApi.postMessage({ type: "saveGraphLayout", layout: readLayout(layout.value) }); }
provideGraphEditor({ layout, save });
function receive(event: MessageEvent<GraphEditorHostMessage>) {
    if (event.data?.type !== "graphState") return;
    state.value = event.data.state;
    if (!initializedLayout) {
        layout.value = readLayout(state.value.layout);
        initializedLayout = true;
    }
    if (state.value.scope) {
        const placed = placeNodes(state.value.scope.nodes, layout.value);
        const added = Object.keys(placed.positions).length !== Object.keys(layout.value.positions).length;
        layout.value = placed;
        if (added) save();
    }
}
onMounted(() => {
    // Give VS Code a persisted panel state so its serializer can restore
    // this editor. Geometry itself lives in the project layout file.
    webviewApi.setState({ ...webviewApi.getState(), surface: "graphEditor" });
    window.addEventListener("message", receive);
    webviewApi.postMessage({ type: "graphReady" });
});
onBeforeUnmount(() => window.removeEventListener("message", receive));
</script>

<template>
    <main class="graph-editor">
        <div v-if="state.sessionStatus !== 'running'" class="status" role="status">
            {{ state.sessionStatus === 'starting' ? 'Starting Intravenous…' : 'Restarting Intravenous…' }}
        </div>
        <div v-if="state.error" class="error" role="alert">{{ state.error }}</div>
        <PresentationHost v-if="state.scope" :node="root" view-choice="editable" />
    </main>
</template>

<style scoped>
.graph-editor { height: 100vh; display: flex; flex-direction: column; background: var(--vscode-editor-background); }
.graph-editor :deep(> .presentation-host) { flex: 1; min-height: 0; }
.status, .error { padding: 8px; }
.error { color: var(--vscode-errorForeground); }
</style>

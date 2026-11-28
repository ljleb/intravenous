<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, useId, watch } from "vue";
import { applyTagCompletion, queryByteOffset, rankNodeDefinitions, type NodeDefinitionCatalog,
    type NodeDefinitionPreview, type NodeDefinitionSearch } from "../../src/nodeDefinitionSearch";

const props = defineProps<{
    catalog: NodeDefinitionCatalog | null;
    usage: Readonly<Record<string, number>>;
    unavailableReason?: (definition: NodeDefinitionPreview) => string | undefined;
    creationError?: string;
    response?: { requestId: string; result?: NodeDefinitionSearch; error?: string };
}>();
const emit = defineEmits<{
    search: [requestId: string, source: string, cursor: number];
    choose: [definition: NodeDefinitionPreview];
    close: [];
}>();

const input = ref<HTMLInputElement>();
const list = ref<HTMLDivElement>();
const suggestions = ref<HTMLDivElement>();
const suggestionsId = useId();
const source = ref("");
const reply = ref<NodeDefinitionSearch>();
const error = ref("");
const selectedId = ref("");
const completionIndex = ref(0);
const completionRequested = ref(false);
let latestRequest = "";
let latestCursor = 0;
let delay: ReturnType<typeof setTimeout> | undefined;

const query = computed(() => reply.value?.generation === props.catalog?.generation ? reply.value : undefined);
const explicitQuery = computed(() => /[()|!=:]|(?:^|\s)id\./.test(source.value));
const rows = computed(() => rankNodeDefinitions(props.catalog?.definitions ?? [], source.value,
    query.value?.matches ?? (explicitQuery.value ? [] : undefined), props.usage));
const active = computed(() => rows.value.find((row) => row.id === selectedId.value) ?? rows.value[0]);
const ports = computed(() => ({ inputs: active.value?.ports.filter((port) => !port.output) ?? [],
    outputs: active.value?.ports.filter((port) => port.output) ?? [] }));
const choices = computed(() => {
    const completion = query.value?.completion;
    if (!completionRequested.value || !completion) return [];
    return completion.candidates.filter((candidate) =>
        applyTagCompletion(source.value, completion.start, completion.end, candidate.insertText).source !== source.value);
});
const completionLabels = computed(() => {
    const completion = query.value?.completion;
    if (!completion || !choices.value.length) return [];
    const bytes = new TextEncoder().encode(source.value);
    const prefix = new TextDecoder().decode(bytes.subarray(completion.start, Math.min(latestCursor, completion.end)));
    return choices.value.map(({ label, insertText }) => {
        if (!prefix || !insertText.startsWith(prefix) || !label.endsWith(insertText))
            return { before: label, match: "", after: "" };
        const start = label.length - insertText.length;
        const end = start + prefix.length;
        return { before: label.slice(0, start), match: label.slice(start, end), after: label.slice(end) };
    });
});

function scheduleSearch() {
    clearTimeout(delay);
    reply.value = undefined;
    completionIndex.value = 0;
    error.value = "";
    latestRequest = "";
    if (!props.catalog || (!source.value.trim() && !completionRequested.value)) return;
    delay = setTimeout(() => {
        latestRequest = crypto.randomUUID();
        latestCursor = queryByteOffset(source.value, input.value?.selectionStart ?? source.value.length);
        emit("search", latestRequest, source.value, latestCursor);
    }, 50);
}

watch(() => props.response, (response) => {
    if (!response || response.requestId !== latestRequest) return;
    reply.value = response.result;
    error.value = response.error ?? "";
    if (!choices.value.length) dismissCompletion();
});
watch(() => props.catalog?.generation, scheduleSearch);
onMounted(() => input.value?.focus());
onUnmounted(() => clearTimeout(delay));

function edit(event: Event) {
    source.value = (event.target as HTMLInputElement).value;
    completionRequested.value = !!source.value.trim();
    selectedId.value = "";
    if (list.value) list.value.scrollTop = 0;
    scheduleSearch();
}

function dismissCompletion() {
    completionRequested.value = false;
}

function complete(index: number) {
    const choice = choices.value[index];
    const completion = query.value?.completion;
    if (!choice || !completion) return;
    const replacement = applyTagCompletion(source.value, completion.start, completion.end, choice.insertText);
    dismissCompletion();
    source.value = replacement.source;
    selectedId.value = "";
    void nextTick(() => {
        input.value?.focus();
        input.value?.setSelectionRange(replacement.caret, replacement.caret);
        scheduleSearch();
    });
}

function choose(node: NodeDefinitionPreview) {
    if (!props.unavailableReason?.(node)) emit("choose", node);
}

function keyDown(event: KeyboardEvent) {
    if (event.isComposing) return;
    if (event.code === "Space" && event.ctrlKey && !event.altKey && !event.metaKey && !event.shiftKey) {
        event.preventDefault();
        if (event.repeat) return;
        completionRequested.value = true;
        scheduleSearch();
    } else if (event.key === "Escape") {
        event.preventDefault();
        if (event.repeat) return;
        if (completionRequested.value) dismissCompletion();
        else emit("close");
    } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        const step = event.key === "ArrowDown" ? 1 : -1;
        if (choices.value.length) {
            completionIndex.value = (completionIndex.value + step + choices.value.length) % choices.value.length;
            void nextTick(() => suggestions.value?.querySelector("[aria-selected=true]")
                ?.scrollIntoView({ block: "nearest" }));
        } else if (rows.value.length) {
            const index = rows.value.findIndex((row) => row.id === active.value?.id);
            selectedId.value = rows.value[(index + step + rows.value.length) % rows.value.length].id;
            void nextTick(() => list.value?.querySelector("[aria-selected=true]")?.scrollIntoView({ block: "nearest" }));
        }
    } else if ((event.key === "Tab" && !event.shiftKey) || event.key === "Enter") {
        event.preventDefault();
        if (choices.value.length) complete(completionIndex.value);
        else if (event.key === "Enter" && active.value) choose(active.value);
    }
}
</script>

<template>
    <Teleport to="body">
        <div class="picker-backdrop" @click.self="emit('close')" @keydown.stop="keyDown">
            <section class="picker" role="dialog" aria-modal="true" aria-label="Add node">
                <div class="search-field">
                    <input ref="input" class="search" :value="source" aria-label="Search node definitions" placeholder="Search nodes"
                        role="combobox" aria-autocomplete="list" :aria-expanded="!!choices.length"
                        :aria-controls="choices.length ? suggestionsId : undefined"
                        :aria-activedescendant="choices.length ? `${suggestionsId}-${completionIndex}` : undefined"
                        maxlength="256" autocomplete="off" spellcheck="false" @input="edit" @click="dismissCompletion"
                        @keyup.left="dismissCompletion" @keyup.right="dismissCompletion" @keyup.home="dismissCompletion" @keyup.end="dismissCompletion" />
                    <div v-if="choices.length" :id="suggestionsId" ref="suggestions" class="completions" role="listbox" aria-label="Tag completions">
                        <button v-for="(label, index) in completionLabels" :id="`${suggestionsId}-${index}`" :key="choices[index].label"
                            type="button" class="completion" role="option" :aria-selected="index === completionIndex" tabindex="-1"
                            @mousedown.prevent @click="complete(index)">
                            <span class="completion-icon" aria-hidden="true">#</span>
                            <span class="completion-label">{{ label.before }}<span v-if="label.match" class="completion-match">{{ label.match }}</span>{{ label.after }}</span>
                        </button>
                    </div>
                </div>
                <div v-if="creationError" class="empty" role="alert">{{ creationError }}</div>
                <div v-else-if="error || (explicitQuery && query?.error && !choices.length)" class="empty" role="status">
                    {{ error || query?.error }}
                </div>
                <div v-else-if="!rows.length && (!explicitQuery || query?.matches)" class="empty" role="status">
                    {{ catalog ? 'No matching nodes' : 'Loading node definitions…' }}
                </div>
                <div ref="list" class="results" role="listbox" aria-label="Node definitions">
                    <button v-for="node in rows" :key="node.id" class="node" role="option" :aria-selected="active?.id === node.id"
                        :aria-disabled="!!unavailableReason?.(node)" type="button" tabindex="-1"
                        @pointermove="selectedId = node.id" @mousedown.prevent @click="choose(node)">
                        <span>{{ node.displayName || node.id }}</span><small>{{ node.package }}</small>
                    </button>
                </div>
                <footer v-if="active" class="preview" aria-label="Node preview">
                    <div class="preview-id">{{ active.id }}</div>
                    <div v-if="active.ports.length" class="ports">
                        <div><span v-for="(port, index) in ports.inputs" :key="index"><i :class="{ event: port.event }" />{{ port.name }}</span></div>
                        <div><span v-for="(port, index) in ports.outputs" :key="index">{{ port.name }}<i :class="{ event: port.event }" /></span></div>
                    </div>
                    <div v-if="active.tags.length" class="preview-tags">{{ active.tags.join(' · ') }}</div>
                    <div v-if="unavailableReason?.(active)" class="preview-tags">{{ unavailableReason?.(active) }}</div>
                </footer>
            </section>
        </div>
    </Teleport>
</template>

<style scoped>
.picker-backdrop { position: fixed; inset: 0; z-index: 40; display: grid; justify-items: center; align-items: start; background: #0005; }
.picker { width: min(530px, calc(100vw - 32px)); max-height: 80vh; margin-top: min(12vh, 80px); display: flex; flex-direction: column; background: var(--vscode-quickInput-background, var(--vscode-editorWidget-background)); color: var(--vscode-foreground); border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); box-shadow: 0 6px 20px var(--vscode-widget-shadow, #0008); }
.search-field { position: relative; flex: 0 0 auto; display: flex; padding: 8px; }
.search { flex: 1; padding: 7px; min-width: 0; border: 1px solid var(--vscode-input-border, transparent); background: var(--vscode-input-background); color: var(--vscode-input-foreground); }
.search:focus { outline: 1px solid var(--vscode-focusBorder); }
.completions {
    position: absolute;
    z-index: 1;
    inset-block-start: calc(100% - 6px);
    inset-inline-start: 8px;
    inline-size: min(420px, calc(100% - 16px));
    max-block-size: min(154px, 44vh);
    box-sizing: border-box;
    overflow: auto;
    background: var(--vscode-editorSuggestWidget-background, var(--vscode-editorWidget-background));
    color: var(--vscode-editorSuggestWidget-foreground, var(--vscode-editorWidget-foreground, var(--vscode-foreground)));
    border: 1px solid var(--vscode-editorSuggestWidget-border, var(--vscode-panel-border));
    box-shadow: 0 3px 9px var(--vscode-widget-shadow, #0005);
    font: var(--vscode-editor-font-size, 13px) / 1.5 var(--vscode-editor-font-family, monospace);
}
.completion, .node { width: 100%; display: flex; text-align: left; border: 0; background: transparent; color: inherit; font: inherit; cursor: pointer; }
.completion { align-items: center; gap: 5px; padding: 1px 6px; }
.completion[aria-selected=true] { background: var(--vscode-editorSuggestWidget-selectedBackground, var(--vscode-list-activeSelectionBackground)); color: var(--vscode-editorSuggestWidget-selectedForeground, var(--vscode-list-activeSelectionForeground)); }
.completion-icon { flex: 0 0 16px; text-align: center; color: var(--vscode-symbolIcon-keywordForeground, currentColor); }
.completion[aria-selected=true] .completion-icon { color: var(--vscode-editorSuggestWidget-selectedIconForeground, var(--vscode-symbolIcon-keywordForeground, currentColor)); }
.completion-label { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.completion-match { color: var(--vscode-editorSuggestWidget-highlightForeground, var(--vscode-list-highlightForeground)); font-weight: 600; }
.completion[aria-selected=true] .completion-match { color: var(--vscode-editorSuggestWidget-focusHighlightForeground, var(--vscode-editorSuggestWidget-highlightForeground, currentColor)); }
.results { flex: 1 1 auto; overflow: auto; }
.node { justify-content: space-between; padding: 6px 10px; }
.node[aria-selected=true] { background: var(--vscode-quickInputList-focusBackground, var(--vscode-list-activeSelectionBackground)); color: var(--vscode-quickInputList-focusForeground, var(--vscode-list-activeSelectionForeground)); }
.node[aria-disabled=true] { cursor: default; }
small, .preview-tags, .preview-id, .empty { color: var(--vscode-descriptionForeground); }
.preview, .empty { padding: 8px 10px; }
.preview { flex: 0 0 auto; border-top: 1px solid var(--vscode-panel-border); }
.preview-id { font: 11px var(--vscode-editor-font-family); overflow-wrap: anywhere; }
.ports { display: flex; justify-content: space-between; gap: 8px; margin-top: 5px; max-height: 92px; overflow: auto; }
.ports span { display: flex; align-items: center; gap: 5px; }
.ports div:last-child span { justify-content: end; }
.ports i { width: 7px; height: 7px; flex: 0 0 7px; border-radius: 50%; background: var(--iv-port-input); }
.ports div:last-child i { background: var(--iv-port-output); }
.ports i.event { border-radius: 2px; }
.preview-tags { margin-top: 5px; font-size: 11px; }
</style>

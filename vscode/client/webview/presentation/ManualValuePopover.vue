<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { ManualControlGestures } from "../../src/manualControlGesture";

const props = defineProps<{ gestures: ManualControlGestures; version: number }>();
const input = ref<HTMLInputElement | null>(null);
const viewport = ref({ width: window.innerWidth, height: window.innerHeight });
const entry = computed(() => { void props.version; return props.gestures.numericEntry(); });
const position = computed(() => {
    const anchor = entry.value?.anchor;
    if (!anchor) return {};
    const width = Math.min(190, viewport.value.width - 8);
    const height = entry.value?.error ? 90 : 42;
    const proposedTop = anchor.bottom + height + 4 <= viewport.value.height ? anchor.bottom + 4 : anchor.top - height - 4;
    const top = Math.max(4, Math.min(proposedTop, viewport.value.height - height - 4));
    return { width: `${width}px`, left: `${Math.max(4, Math.min(anchor.left, viewport.value.width - width - 4))}px`,
        top: `${top}px`, maxHeight: `${Math.max(20, viewport.value.height - top - 4)}px` };
});
function resize() { viewport.value = { width: window.innerWidth, height: window.innerHeight }; }
function close(accept: boolean) {
    if (accept && !props.gestures.acceptEntry()) return;
    if (!accept) props.gestures.cancelEntry();
    document.querySelector<HTMLElement>(".graph-editor")?.focus({ preventScroll: true });
}
function blur() {
    queueMicrotask(() => {
        if (input.value?.isConnected && document.activeElement !== input.value) props.gestures.cancelEntry();
    });
}
watch(() => entry.value?.id, async (id) => {
    if (!id) return;
    await nextTick();
    input.value?.focus({ preventScroll: true });
    input.value?.select();
});
onMounted(() => window.addEventListener("resize", resize));
onBeforeUnmount(() => window.removeEventListener("resize", resize));
</script>

<template>
    <Teleport to="body">
        <form v-if="entry" class="manual-value-popover" :style="position" role="dialog"
            :aria-label="`${entry.control.inputName} value`" @submit.prevent="close(true)"
            @pointerdown.stop @keydown.esc.stop.prevent="close(false)">
            <input ref="input" type="text" inputmode="decimal" :value="entry.draft"
                :aria-label="`${entry.control.inputName} value`" :aria-invalid="!!entry.error"
                :title="entry.error || undefined" autocomplete="off" spellcheck="false"
                @input="props.gestures.setEntryDraft(($event.target as HTMLInputElement).value)" @blur="blur" />
            <span v-if="entry.error" class="entry-error" role="alert">{{ entry.error }}</span>
        </form>
    </Teleport>
</template>

<style scoped>
.manual-value-popover { position: fixed; z-index: 50; box-sizing: border-box; padding: 6px;
    overflow-y: auto;
    border: 1px solid var(--vscode-panel-border); border-radius: 5px;
    background: var(--vscode-editorWidget-background); box-shadow: 0 3px 12px #0005; }
input { box-sizing: border-box; width: 100%; min-width: 0; padding: 4px;
    border: 1px solid var(--vscode-focusBorder); outline: none;
    background: var(--vscode-input-background); color: var(--vscode-input-foreground); }
input[aria-invalid="true"] { border-color: var(--vscode-inputValidation-errorBorder, var(--vscode-errorForeground)); }
.entry-error { display: block; margin-top: 4px; font-size: 11px; color: var(--vscode-errorForeground); }
</style>

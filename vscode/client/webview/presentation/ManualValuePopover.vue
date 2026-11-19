<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { KnobValueEntry } from "../../src/knobValueEntry";

const props = defineProps<{ entry: KnobValueEntry | null; accept: () => boolean; cancel: () => void;
    setDraft: (draft: string) => void; target?: HTMLElement; returnFocus?: HTMLElement }>();
const input = ref<HTMLInputElement | null>(null);
const viewport = ref({ width: window.innerWidth, height: window.innerHeight });
const position = computed(() => {
    const anchor = props.entry?.anchor;
    if (!anchor) return {};
    const width = Math.min(190, viewport.value.width - 8);
    const height = props.entry?.error ? 90 : 42;
    const proposedTop = anchor.bottom + height + 4 <= viewport.value.height ? anchor.bottom + 4 : anchor.top - height - 4;
    const top = Math.max(4, Math.min(proposedTop, viewport.value.height - height - 4));
    return { width: `${width}px`, left: `${Math.max(4, Math.min(anchor.left, viewport.value.width - width - 4))}px`,
        top: `${top}px`, maxHeight: `${Math.max(20, viewport.value.height - top - 4)}px` };
});
function resize() { viewport.value = { width: window.innerWidth, height: window.innerHeight }; }
function close(accept: boolean) {
    if (accept && !props.accept()) return;
    if (!accept) props.cancel();
    const target = props.returnFocus ?? document.querySelector<HTMLElement>(".graph-editor");
    target?.focus({ preventScroll: true });
}
function blur() {
    const id = props.entry?.id;
    queueMicrotask(() => {
        if (props.entry?.id === id && input.value?.isConnected && document.activeElement !== input.value) props.cancel();
    });
}
watch(() => props.entry?.id, async (id) => {
    if (!id) return;
    await nextTick();
    input.value?.focus({ preventScroll: true });
    input.value?.select();
});
onMounted(() => window.addEventListener("resize", resize));
onBeforeUnmount(() => window.removeEventListener("resize", resize));
</script>

<template>
    <Teleport :to="target ?? 'body'">
        <form v-if="entry" class="manual-value-popover" :style="position" role="dialog"
            :aria-label="`${entry.label} value`" @submit.prevent="close(true)"
            @pointerdown.stop @keydown.esc.stop.prevent="close(false)">
            <input ref="input" type="text" inputmode="decimal" :value="entry.draft"
                :aria-label="`${entry.label} value`" :aria-invalid="!!entry.error"
                :aria-description="entry.error || undefined"
                autocomplete="off" spellcheck="false"
                @input="setDraft(($event.target as HTMLInputElement).value)" @blur="blur" />
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

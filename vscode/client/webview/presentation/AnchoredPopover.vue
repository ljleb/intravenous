<script setup lang="ts">
import { onBeforeUnmount, ref, type CSSProperties } from "vue";
import { useOptionalGraphEditor } from "../graph/context";

const props = defineProps<{ id: string; label: string; width: number; preferredHeight: number }>();
const emit = defineEmits<{ toggle: [open: boolean] }>();
const panel = ref<HTMLElement | null>(null);
const editor = useOptionalGraphEditor();
const position = ref<CSSProperties>({});
function place(anchor: HTMLElement) {
    const rect = anchor.getBoundingClientRect();
    const below = window.innerHeight - rect.bottom, above = rect.top;
    const placeBelow = below >= props.preferredHeight || below >= above;
    const width = Math.min(props.width, window.innerWidth - 16);
    position.value = {
        width: `${width}px`, left: `${Math.max(8, Math.min(rect.right - width, window.innerWidth - width - 8))}px`,
        top: placeBelow ? `${rect.bottom + 4}px` : "auto",
        bottom: placeBelow ? "auto" : `${window.innerHeight - rect.top + 4}px`,
        maxHeight: `${Math.max(0, (placeBelow ? below : above) - 8)}px`,
    };
}
function close() { if (panel.value?.matches(":popover-open")) panel.value.hidePopover(); }
function toggled(event: ToggleEvent) {
    const open = event.newState === "open";
    if (!open && panel.value) editor?.closeKnobValueWithin(panel.value);
    emit("toggle", open);
}
onBeforeUnmount(() => { if (panel.value) editor?.closeKnobValueWithin(panel.value); });
defineExpose({ place, close });
</script>

<template>
    <div :id="id" ref="panel" class="anchored-popover" popover="auto" :style="position" role="dialog" :aria-label="label"
        @toggle="toggled" @keydown.stop @pointerdown.stop @wheel.stop>
        <slot />
    </div>
</template>

<style scoped>
.anchored-popover {
    position: fixed; inset: auto; margin: 0; box-sizing: border-box; overflow: auto; padding: 10px;
    color: var(--vscode-editorWidget-foreground); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); box-shadow: 0 2px 8px var(--vscode-widget-shadow);
}
.anchored-popover:popover-open { display: grid; gap: 8px; }
</style>

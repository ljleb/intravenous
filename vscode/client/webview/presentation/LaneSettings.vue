<script setup lang="ts">
import { ref, useId } from "vue";
import AnchoredPopover from "./AnchoredPopover.vue";

defineProps<{ label: string; dialogLabel: string; hint: string; width: number; preferredHeight: number }>();
const emit = defineEmits<{ toggle: [open: boolean] }>();
const id = useId();
const panel = ref<InstanceType<typeof AnchoredPopover> | null>(null);
const open = ref(false);
function place(event: MouseEvent) {
    const button = event.currentTarget as HTMLElement;
    if (event.detail > 0 && document.activeElement === button) button.blur();
    panel.value?.place(button);
}
function close() { panel.value?.close(); }
function toggled(visible: boolean) { open.value = visible; emit("toggle", visible); }
</script>

<template>
    <button type="button" class="lane-settings-button" :popovertarget="id" :aria-label="dialogLabel" aria-haspopup="dialog" :aria-expanded="open"
        :data-status-hint="hint" @pointerdown.stop @mousedown.left.prevent @click.stop="place" @keydown.space.stop>{{ label }}</button>
    <AnchoredPopover :id="id" ref="panel" :label="dialogLabel" :width="width" :preferred-height="preferredHeight"
        @toggle="toggled"><slot :close="close" /></AnchoredPopover>
</template>

<style scoped>
.lane-settings-button {
    position: absolute; top: 2px; right: 4px; max-width: calc(100% - 8px); overflow: hidden; text-overflow: ellipsis;
    border: 1px solid transparent; padding: 1px 4px; border-radius: 3px; white-space: nowrap;
    color: var(--vscode-descriptionForeground); background: var(--vscode-editor-background); font: inherit; font-size: 10px; cursor: pointer;
}
.lane-settings-button:is(:hover, [aria-expanded="true"]) { color: var(--vscode-foreground); }
.lane-settings-button:focus:not(:focus-visible) { outline: none; }
.lane-settings-button:focus-visible { outline: 1px solid var(--vscode-focusBorder); }
</style>

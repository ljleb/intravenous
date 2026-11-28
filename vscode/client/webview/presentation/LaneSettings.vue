<script setup lang="ts">
import { ref, useId } from "vue";
import AnchoredPopover from "./AnchoredPopover.vue";

defineProps<{ label: string; dialogLabel: string; hint: string; width: number; preferredHeight: number }>();
const emit = defineEmits<{ toggle: [open: boolean] }>();
const id = useId();
const panel = ref<InstanceType<typeof AnchoredPopover> | null>(null);
const open = ref(false);
function place(event: MouseEvent) {
    panel.value?.place(event.currentTarget as HTMLElement);
}
function close() { panel.value?.close(); }
function toggled(visible: boolean) { open.value = visible; emit("toggle", visible); }
</script>

<template>
    <button type="button" tabindex="-1" class="lane-settings-button" :popovertarget="id" :aria-label="dialogLabel" aria-haspopup="dialog" :aria-expanded="open"
        :data-status-hint="hint" @pointerdown.stop @mousedown.left.prevent @click.stop="place" @keydown.space.stop>
        <span class="iv-text-link">{{ label }}</span>
    </button>
    <AnchoredPopover :id="id" ref="panel" :label="dialogLabel" :width="width" :preferred-height="preferredHeight"
        @toggle="toggled"><slot :close="close" /></AnchoredPopover>
</template>

<style scoped>
.lane-settings-button {
    position: absolute; top: 0; right: 0; max-width: 100%; box-sizing: border-box;
    border: 0; padding: 2px 4px 4px 10px; background: transparent; font: inherit; font-size: 10px; cursor: pointer;
}
.lane-settings-button span {
    display: block; overflow: hidden; text-overflow: ellipsis;
    border: 1px solid transparent; padding: 1px 4px; border-radius: 3px; white-space: nowrap;
    background: var(--vscode-editor-background);
}
.lane-settings-button[aria-expanded="true"] span { --iv-text-link-color: var(--vscode-foreground); }
</style>

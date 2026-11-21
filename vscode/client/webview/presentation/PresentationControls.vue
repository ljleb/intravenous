<script setup lang="ts">
import type { PresentationControl, PresentationInput } from "../../src/presentationPlugin";
import Knob from "./Knob.vue";
import ContextMenuItem from "./ContextMenuItem.vue";

defineProps<{ rows: readonly PresentationControl[][]; identity: string; editable: boolean; contextual?: boolean }>();
const emit = defineEmits<{ input: [Extract<PresentationInput, { type: "control" }>] }>();
function control(id: string, phase: "preview" | "commit" | "finish", value?: number | string) {
    emit("input", { type: "control", id, phase, value });
}
</script>

<template>
    <div v-for="row in rows" :key="row[0]?.id" class="presentation-control-row">
        <template v-for="item in row" :key="item.id">
            <span v-if="item.type === 'text'">{{ item.text }}</span>
            <Knob v-else-if="item.type === 'knob'" :identity="`${identity}:${item.id}`" :label="item.label"
                :value="item.value" :disabled="!editable || item.max < item.min"
                :limits="{ min: item.min, max: item.max, defaultValue: item.defaultValue,
                    wholeByDefault: item.integer, scrubSpeed: item.scrubSpeed ?? (contextual ? undefined : .5) }" :step="item.step"
                :face-limits="{ min: item.min, defaultValue: item.defaultValue }"
                :apply="(value, live) => control(item.id, live ? 'preview' : 'commit', value)" :end-edit="() => control(item.id, 'finish')" />
            <label v-else-if="item.type === 'choice'">{{ item.label }}
                <select :disabled="!editable" :value="item.value" tabindex="-1"
                    @change="control(item.id, 'commit', ($event.target as HTMLSelectElement).value)">
                    <option v-for="option in item.options" :key="option.value" :value="option.value">{{ option.label }}</option>
                </select>
            </label>
            <ContextMenuItem v-else-if="contextual" :disabled="!editable || item.disabled"
                @click="control(item.id, 'commit')">{{ item.label }}</ContextMenuItem>
            <button v-else type="button" tabindex="-1" :disabled="!editable || item.disabled"
                @click="control(item.id, 'commit')">{{ item.label }}</button>
        </template>
    </div>
</template>

<style scoped>
.presentation-control-row { display: flex; align-items: center; gap: 6px; --knob-size: 32px; }
.presentation-control-row > span:first-child { flex: 1; }
.presentation-control-row > button:not(.knob-control, .context-menu-item) { border: 0; border-radius: 3px; padding: 3px 6px;
    font: inherit; background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground); }
.presentation-control-row > button:disabled { opacity: .4; }
</style>

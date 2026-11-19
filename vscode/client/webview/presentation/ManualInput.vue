<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from "vue";
import { useManualInput } from "./manualControlContext";
import { knobPortStatus } from "../../src/knobPresentation";
import Knob from "./Knob.vue";

const props = defineProps<{ name: string; showName?: boolean; direction?: "input" | "output"; portCutout?: boolean }>();
const control = useManualInput(() => props.name, () => props.direction ?? "input");
const { value, available, limits, state, descriptor, activity } = control;
const inactive = computed(() => !descriptor.value?.participates
    || (props.direction === "output" && value.value === 1 && !control.inverted.value));
const knob = ref<InstanceType<typeof Knob> | null>(null);
const description = computed(() => [knobPortStatus(control.port.value),
    activity.value === "retained" ? "Override held" : !descriptor.value?.enabled && activity.value !== "active" ? "Manual value off" : "",
].filter(Boolean).join(" · "));
function apply(value: number) {
    control.hover(true);
    control.edit(); control.update(value); control.finish();
}
onBeforeUnmount(() => control.hover(false));
</script>

<template>
    <div class="manual-input" :class="{ inactive }"
        :data-direction="direction ?? 'input'"
        :data-activity="activity"
        :data-manual-control="control.key.value"
        :data-status-hint="description" @pointerdown.stop @click.stop @contextmenu="knob?.entry($event)"
        @iv-hoverenter="control.hover(true)" @iv-hoverleave="control.hover(false)">
        <span v-if="showName" class="manual-name">{{ name }}</span>
        <button v-if="direction !== 'output'" type="button" class="knob-side-toggle knob-enable-toggle manual-input-toggle"
            :aria-pressed="!!descriptor?.enabled" :disabled="!descriptor?.available || descriptor.live || control.dragging.value"
            :aria-label="`${name} manual value`" :data-status-hint="descriptor?.enabled ? 'Disable manual value' : 'Enable manual value'"
            @pointerdown.stop @mousedown.left.prevent @click.stop="control.toggleEnabled" @contextmenu.stop.prevent>
            <span aria-hidden="true">{{ descriptor?.enabled ? '●' : '○' }}</span>
        </button>
        <Knob ref="knob" class="manual-knob" :identity="control.key.value" :label="name" :value="value" :limits="limits"
            :disabled="!available" :port-cutout="portCutout" :direction="direction" :activity="activity"
            :inactive="inactive" :inverted="control.inverted.value" :description="description"
            :apply="apply" :bind-drag="control.scrub" :bind-value-entry="control.numeric" :reset="control.reset" @hover="control.hover" />
        <button v-if="direction === 'output'" type="button" class="knob-side-toggle"
            :aria-pressed="control.inverted.value"
            :disabled="!available" :aria-label="`${name}: invert output polarity`"
            :data-status-hint="control.inverted.value ? 'Restore polarity' : 'Invert polarity'"
            aria-description="Changes the sign and keeps the magnitude"
            @pointerdown.stop @mousedown.left.prevent @click.stop="control.invert" @contextmenu.stop.prevent
            @focus="control.hover(true)" @blur="control.hover(false)">
            <span aria-hidden="true">{{ control.inverted.value ? '−' : '+' }}</span>
        </button>
        <span v-if="state?.error" class="manual-error" :data-status-hint="state.error"
            role="img" :aria-label="`Manual control error. ${state.error}`">!</span>
    </div>
</template>

<style scoped>
.manual-input { display: flex; flex: 0 0 auto; align-items: center; gap: 4px; min-width: 0; padding: 0 4px; }
.manual-input[data-direction="output"] { position: relative; gap: 0; }
.manual-input-toggle { --knob-toggle-accent: var(--iv-port-input); margin-inline-start: 0;
    margin-inline-end: calc(-1 * var(--output-polarity-inset, 2px)); }
.manual-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.inactive .manual-name { color: var(--vscode-descriptionForeground); }
.manual-error { color: var(--vscode-errorForeground); }
.manual-input[data-direction="output"] .manual-error { position: absolute; top: 0; left: 0; font-size: 11px; line-height: 1; }
</style>

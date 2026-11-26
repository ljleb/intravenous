<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from "vue";
import { useManualInput } from "./manualControlContext";
import { knobPortStatus } from "../../src/knobPresentation";
import { portDisplayName } from "./nodeSummary";
import Knob from "./Knob.vue";
import PortLabel from "./PortLabel.vue";

const props = defineProps<{ name: string; showName?: boolean; direction?: "input" | "output"; portCutout?: boolean }>();
const control = useManualInput(() => props.name, () => props.direction ?? "input");
const { value, available, limits, state, descriptor, activity } = control;
const label = computed(() => control.port.value ? portDisplayName(control.port.value) : props.name);
const inactive = computed(() => !descriptor.value?.participates
    || (props.direction === "output" && value.value === 1 && !control.inverted.value));
const knob = ref<InstanceType<typeof Knob> | null>(null);
const description = computed(() => [knobPortStatus(control.port.value),
    state.value?.phase === "starting" || state.value?.phase === "ending" ? "Applying value…" : "",
    activity.value === "retained" ? "Override held" : !descriptor.value?.enabled && activity.value !== "active" ? "Manual value off" : "",
].filter(Boolean).join(" · "));
function apply(value: number) {
    control.hover(true);
    control.applyValue(value);
}
const clipboard = computed(() => ({
    value: control.signedValue.value,
    range: props.direction === "output" ? {} : limits.value,
    apply: (value: number) => control.applyValue(value, true),
}));
onBeforeUnmount(() => control.hover(false));
</script>

<template>
    <div class="manual-input" :class="{ inactive }"
        :data-direction="direction ?? 'input'"
        :data-activity="activity"
        :data-manual-control="control.key.value"
        :data-status-hint="[label, description].filter(Boolean).join(' · ')"
        @pointerdown.stop @click.stop @contextmenu="knob?.entry($event)"
        @iv-hoverenter="control.hover(true)" @iv-hoverleave="control.hover(false)">
        <PortLabel v-if="showName" class="manual-name" :name="label" :short-name="control.port.value?.shortDisplayName" />
        <button v-if="control.canToggleEnabled.value" type="button" tabindex="-1" class="knob-side-toggle knob-enable-toggle manual-input-toggle"
            :aria-pressed="!!descriptor?.enabled" :disabled="!descriptor?.available || descriptor.live || control.dragging.value"
            :aria-label="`${label} manual value`"
            :data-status-hint="`${label} · ${descriptor?.enabled ? 'Disable knob' : 'Enable knob'}`"
            @pointerdown.stop @mousedown.left.prevent @click.stop="control.toggleEnabled" @contextmenu.stop.prevent>
            <span aria-hidden="true" />
        </button>
        <Knob ref="knob" class="manual-knob" :identity="control.key.value" :label="label" :value="value" :limits="limits"
            :disabled="!available" :port-cutout="portCutout" :direction="direction" :activity="activity"
            :inactive="inactive" :inverted="control.inverted.value" :description="description"
            :apply="apply" :clipboard="clipboard" :bind-drag="control.scrub" :bind-value-entry="control.numeric"
            :reset="control.reset" @hover="control.hover" />
        <button v-if="direction === 'output'" type="button" tabindex="-1" class="knob-side-toggle"
            :aria-pressed="control.inverted.value"
            :disabled="!available" :aria-label="`${label}: invert output polarity`"
            :data-status-hint="`${label} · ${control.inverted.value ? 'Restore polarity' : 'Invert polarity'}`"
            aria-description="Changes the sign and keeps the magnitude"
            @pointerdown.stop @mousedown.left.prevent @click.stop="control.invert" @contextmenu.stop.prevent
            @focus="control.hover(true)" @blur="control.hover(false)">
            <span aria-hidden="true" />
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

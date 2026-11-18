<script setup lang="ts">
import { computed } from "vue";
import type { GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { SemanticPort } from "./semanticNode";
import { portDescription } from "./nodeSummary";
import ManualInput from "./ManualInput.vue";

const props = defineProps<{ port: SemanticPort; endpoint: GraphPortEndpoint; height: number; knobSize: number;
    hasKnob: boolean; connected: boolean; pending: boolean; unavailable: boolean;
    hint: string; overflowSlot?: string; expanded?: boolean; continuation?: boolean }>();
const emit = defineEmits<{
    handleHover: [event: Event]; handleLeave: []; wireStart: [event: PointerEvent]; overflow: []; focusOverflow: [];
}>();

const portColumnWidth = 24;
const shellWidth = computed(() => props.knobSize + portColumnWidth);
const shellPath = computed(() => {
    const center = props.knobSize / 2;
    const knobRadius = center - 1;
    const portRadius = 9;
    const knobContact = knobRadius * Math.SQRT1_2;
    const portContact = portRadius * Math.SQRT1_2;
    // The cubics leave both circles tangentially, forming a smooth waist.
    const knobX = center + knobContact;
    const portX = props.knobSize + portColumnWidth / 2 - portContact;
    const handle = (portX - knobX) / 3;
    const knobTop = center - knobContact;
    const portTop = center - portContact;
    const knobBottom = center + knobContact;
    const portBottom = center + portContact;
    return [
        `M ${knobX} ${knobTop}`,
        `C ${knobX + handle} ${knobTop + handle}, ${portX - handle} ${portTop + handle}, ${portX} ${portTop}`,
        `A ${portRadius} ${portRadius} 0 1 1 ${portX} ${portBottom}`,
        `C ${portX - handle} ${portBottom - handle}, ${knobX + handle} ${knobBottom - handle}, ${knobX} ${knobBottom}`,
        `A ${knobRadius} ${knobRadius} 0 1 1 ${knobX} ${knobTop}`,
        "Z",
    ].join(" ");
});
</script>

<template>
    <div class="control-row" :class="{ unavailable }" :data-direction="port.direction"
        :data-port-kind="endpoint.kind" :data-port-drop-target="port.direction === 'input' ? '' : undefined"
        :data-folded-ports="overflowSlot ? port.direction : undefined"
        :style="{ height: `${height}px`, '--knob-size': `${knobSize}px` }"
        @iv-hoverenter="overflowSlot && emit('overflow')" @focusin="overflowSlot && emit('overflow')">
        <svg v-if="hasKnob" class="control-shell" :viewBox="`0 0 ${shellWidth} ${knobSize}`"
            :width="shellWidth" :height="knobSize" aria-hidden="true">
            <path :d="shellPath" :transform="port.direction === 'output' ? `translate(${shellWidth} 0) scale(-1 1)` : undefined" />
        </svg>
        <ManualInput v-if="hasKnob" class="port-knob" :name="port.name" :direction="port.direction" />
        <button type="button" class="port-handle" :class="{ connected, pending }"
            :data-port-endpoint="JSON.stringify(endpoint)" :data-port-slot="overflowSlot"
            :data-status-hint="hint" :aria-description="portDescription(port)"
            :aria-disabled="unavailable" :aria-label="`${port.direction} ${port.name || port.index + 1}`"
            :aria-expanded="overflowSlot ? expanded : undefined"
            @keydown.down="overflowSlot && ($event.preventDefault(), emit('focusOverflow'))"
            @iv-hoverenter="emit('handleHover', $event)" @pointermove="emit('handleHover', $event)"
            @iv-hoverleave="emit('handleLeave')" @pointerdown="emit('wireStart', $event)">
            <span class="port-circle" />
        </button>
        <span v-if="port.direction === 'input' || continuation" class="direct-port-name" :data-status-hint="hint">
            {{ port.name || port.index + 1 }}
        </span>
    </div>
</template>

<style scoped>
.control-row { display: grid; grid-template-columns: var(--knob-size) 24px minmax(0, 1fr);
    align-items: center; min-width: 0; flex-shrink: 0; --port-color: var(--iv-sample-input); }
.control-row[data-direction="output"] { grid-template-columns: 24px max-content minmax(0, 1fr); --port-color: var(--iv-sample-output); }
.control-row[data-port-kind="event"][data-direction="input"] { --port-color: var(--iv-event-input); }
.control-row[data-port-kind="event"][data-direction="output"] { --port-color: var(--iv-event-output); }
.control-row.unavailable { --port-color: var(--vscode-disabledForeground); color: var(--vscode-disabledForeground); }
.control-shell { grid-column: 1 / 3; grid-row: 1; justify-self: start; display: block; pointer-events: none;
    fill: color-mix(in srgb, var(--port-color) 12%, transparent);
    stroke: color-mix(in srgb, var(--port-color) 45%, transparent); stroke-width: .8; }
.port-knob { grid-column: 1; grid-row: 1; padding: 0; gap: 2px; }
.port-handle { grid-column: 2; grid-row: 1; display: grid; place-items: center; width: 24px; height: 22px;
    padding: 0; border: 0; border-radius: 5px; background: transparent; color: var(--port-color); cursor: crosshair; touch-action: none; }
[data-direction="output"] .port-knob { grid-column: 2; }
[data-direction="output"] .port-handle { grid-column: 1; }
.port-circle { width: 12px; height: 12px; border: 2px solid currentColor; border-radius: 50%; box-sizing: border-box; }
[data-port-kind="event"] .port-circle { border-radius: 3px; }
.connected .port-circle { background: currentColor; }
.pending .port-circle { border-style: dashed; }
.port-handle[data-hovered] { background: color-mix(in srgb, currentColor 12%, transparent); }
.port-handle:focus-visible { outline: 1px solid currentColor; outline-offset: -1px; }
.direct-port-name { grid-column: 3; grid-row: 1; padding-right: 3px; min-width: 0; overflow: hidden;
    text-overflow: ellipsis; white-space: nowrap; font-size: 11px; }
</style>

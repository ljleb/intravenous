<script setup lang="ts">
import type { GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { SemanticPort } from "./semanticNode";
import { portDescription } from "./nodeSummary";
import ManualInput from "./ManualInput.vue";

defineProps<{ port: SemanticPort; endpoint: GraphPortEndpoint; height: number; knobSize: number;
    hasKnob: boolean; connected: boolean; pending: boolean; unavailable: boolean;
    hint: string; overflowSlot?: string; expanded?: boolean; continuation?: boolean }>();
const emit = defineEmits<{
    handleHover: [event: Event]; handleLeave: []; wireStart: [event: PointerEvent]; overflow: []; focusOverflow: [];
}>();
</script>

<template>
    <div class="control-row" :class="{ unavailable, 'with-knob': hasKnob }" :data-direction="port.direction"
        :data-port-kind="endpoint.kind" :data-port-drop-target="port.direction === 'input' ? '' : undefined"
        :data-folded-ports="overflowSlot ? port.direction : undefined"
        :style="{ height: `${height}px`, '--knob-size': `${knobSize}px` }"
        @iv-hoverenter="overflowSlot && emit('overflow')" @focusin="overflowSlot && emit('overflow')">
        <ManualInput v-if="hasKnob" class="port-knob" :name="port.name" :direction="port.direction" port-cutout />
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
.port-knob { grid-column: 1; grid-row: 1; padding: 0; gap: 2px; }
.port-handle { grid-column: 2; grid-row: 1; display: grid; place-items: center; width: 24px; height: 22px;
    padding: 0; border: 0; border-radius: 5px; background: transparent; color: var(--port-color); cursor: crosshair; touch-action: none; }
[data-direction="output"] .port-knob { grid-column: 2; }
[data-direction="output"] .port-handle { grid-column: 1; }
.port-circle { width: 12px; height: 12px; border: 2px solid currentColor; border-radius: 50%; box-sizing: border-box; }
[data-port-kind="event"] .port-circle { border-radius: 3px; }
/* Cut past the outer edge of the dial track (12.75 units in its 32-unit viewBox). */
.control-row.with-knob {
    isolation: isolate;
    --socket-tuck: 7px;
    --cutout-center: calc(var(--socket-tuck) - var(--knob-size) / 2);
    --cutout-radius: calc(var(--knob-size) * .3984375 + 1.5px);
}
.with-knob .port-knob { position: relative; z-index: 1; }
.with-knob .port-handle { position: relative; left: -4px; overflow: visible; }
.with-knob .port-handle::after { content: ""; position: absolute; inset: 0; z-index: 2; }
.with-knob .port-circle {
    justify-self: start;
    transform: translateX(calc(4px - var(--socket-tuck)));
    background: var(--vscode-editor-background);
    --socket-mask: radial-gradient(circle at var(--cutout-center) 50%,
        transparent calc(var(--cutout-radius) - .5px), #000 calc(var(--cutout-radius) + .5px));
    -webkit-mask-image: var(--socket-mask);
    mask-image: var(--socket-mask);
}
[data-direction="output"].with-knob .port-handle { left: 4px; }
[data-direction="output"].with-knob .port-circle {
    --cutout-center: calc(100% - var(--socket-tuck) + var(--knob-size) / 2);
    justify-self: end;
    transform: translateX(calc(var(--socket-tuck) - 4px));
}
.connected .port-circle { background: currentColor; }
.pending .port-circle { border-style: dashed; }
.port-handle[data-hovered] { background: color-mix(in srgb, currentColor 12%, transparent); }
.with-knob .port-handle[data-hovered] { background: transparent; }
.with-knob .port-handle[data-hovered] .port-circle { filter: brightness(1.25); }
.port-handle:focus-visible { outline: 1px solid currentColor; outline-offset: -1px; }
.direct-port-name { grid-column: 3; grid-row: 1; padding-right: 3px; min-width: 0; overflow: hidden;
    text-overflow: ellipsis; white-space: nowrap; font-size: 11px; }
</style>

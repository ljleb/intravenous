<script setup lang="ts">
import type { GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { SemanticPort } from "./semanticNode";
import { portDescription } from "./nodeSummary";
import ManualInput from "./ManualInput.vue";

defineProps<{ port: SemanticPort; endpoint: GraphPortEndpoint;
    hasKnob: boolean; connected: boolean; pending: boolean; unavailable: boolean;
    hint: string }>();
const emit = defineEmits<{
    handleHover: [event: Event]; handleLeave: []; wireStart: [event: PointerEvent];
}>();
</script>

<template>
    <div class="control-row" :class="{ unavailable, 'with-knob': hasKnob }" :data-direction="port.direction"
        :data-port-kind="endpoint.kind" :data-port-drop-target="port.direction === 'input' ? '' : undefined">
        <ManualInput v-if="hasKnob" class="port-knob" :name="port.name" :direction="port.direction" port-cutout />
        <button type="button" tabindex="-1" class="port-handle" :class="{ connected, pending }"
            :data-port-endpoint="JSON.stringify(endpoint)" :data-status-hint="hint" :aria-description="portDescription(port)"
            :aria-disabled="unavailable" :aria-label="`${port.direction} ${port.displayName || port.name || port.index + 1}`"
            @iv-hoverenter="emit('handleHover', $event)" @pointermove="emit('handleHover', $event)"
            @iv-hoverleave="emit('handleLeave')" @pointerdown="emit('wireStart', $event)">
            <span class="port-circle" />
        </button>
        <span class="direct-port-name" :data-status-hint="hint">
            {{ port.displayName || port.name || port.index + 1 }}
        </span>
    </div>
</template>

<style scoped>
.control-row {
    --knob-size: var(--port-row-height);
    --handle-shift: calc(-1 * var(--socket-tuck) + (var(--port-socket-size) - var(--port-handle-width)) / 2);
    /* Keep the socket fixed even when the hit area shifts away from the knob. */
    --socket-shift: calc(-1 * var(--socket-tuck) - var(--handle-shift));
    display: grid; grid-template-columns: var(--port-control-column) var(--port-handle-width) minmax(0, 1fr);
    grid-template-rows: var(--port-row-height);
    align-items: center; min-width: 0; flex-shrink: 0; --port-color: var(--iv-port-input); }
.control-row[data-direction="output"] { grid-template-columns: minmax(0, 1fr) var(--port-handle-width) var(--port-control-column);
    --port-color: var(--iv-port-output); }
.control-row.unavailable { --port-color: var(--vscode-disabledForeground); color: var(--vscode-disabledForeground); }
.port-knob { grid-column: 1; grid-row: 1; padding: 0; gap: 2px; }
.port-handle { position: relative; left: var(--handle-shift); grid-column: 2; grid-row: 1; display: grid;
    align-items: center; width: var(--port-handle-width); height: 22px; overflow: visible;
    padding: 0; border: 0; border-radius: 5px; background: transparent; color: var(--port-color); cursor: crosshair; touch-action: none; }
[data-direction="output"] .port-knob { grid-column: 3; }
.port-circle {
    --port-circle-fill: color-mix(in srgb, currentColor var(--iv-disconnected-port-strength), var(--vscode-editor-background));
    width: var(--port-socket-size); height: var(--port-socket-size); border-radius: 50%; box-sizing: border-box;
    justify-self: start; transform: translateX(var(--socket-shift));
    background: var(--port-circle-fill);
}
[data-port-kind="event"] .port-circle { border-radius: 3px; }
/* Cut past the outer edge of the dial track (12.75 units in its 32-unit viewBox). */
.control-row.with-knob {
    isolation: isolate;
    --handle-shift: -4px;
    --cutout-center: calc(var(--socket-tuck) - var(--knob-size) / 2);
    --cutout-radius: calc(var(--knob-size) * .3984375 + 1.5px);
}
.with-knob .port-knob { position: relative; z-index: 1; gap: 0; }
.with-knob .port-handle::after { content: ""; position: absolute; inset: 0; z-index: 2; }
.with-knob .port-circle {
    --socket-mask: radial-gradient(circle at var(--cutout-center) 50%,
        transparent calc(var(--cutout-radius) - .5px), #000 calc(var(--cutout-radius) + .5px));
    -webkit-mask-image: var(--socket-mask);
    mask-image: var(--socket-mask);
}
[data-direction="output"] .port-handle { left: calc(-1 * var(--handle-shift)); }
[data-direction="output"] .port-circle {
    --cutout-center: calc(100% - var(--socket-tuck) + var(--knob-size) / 2);
    justify-self: end;
    transform: translateX(calc(-1 * var(--socket-shift)));
}
.connected .port-circle { --port-circle-fill: currentColor; }
.pending .port-circle { border: 2px dashed currentColor; }
.port-handle[data-hovered] { background: color-mix(in srgb, currentColor 12%, transparent); }
.with-knob .port-handle[data-hovered] { background: transparent; }
.with-knob .port-handle[data-hovered] .port-circle { filter: brightness(1.25); }
.direct-port-name { grid-column: 3; grid-row: 1; padding-right: 3px; min-width: 0; overflow: hidden;
    text-overflow: ellipsis; white-space: nowrap; font-size: 11px;
    color: color-mix(in srgb, var(--port-color) var(--iv-port-label-accent), var(--iv-port-label-base)); }
.unavailable .direct-port-name { color: var(--vscode-disabledForeground); }
[data-direction="output"] .direct-port-name { grid-column: 1; text-align: right; }
.with-knob .direct-port-name { align-self: start; margin-block-start: 2px; margin-inline-start: -17px; line-height: 13px; }
[data-direction="output"].with-knob .direct-port-name {
    margin-inline-start: 0; margin-inline-end: -17px; padding-right: 0;
}
</style>

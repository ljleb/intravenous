<script setup lang="ts">
import type { GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { ConnectionFeedback } from "../../src/graphScope";
import PortLabel from "./PortLabel.vue";

defineProps<{
    endpoint: GraphPortEndpoint;
    connectionId?: string;
    label: string;
    shortLabel?: string;
    description: string;
    hint: string;
    connected: boolean;
    pending: boolean;
    unavailable?: boolean;
    connectionChooser?: boolean;
    feedback?: ConnectionFeedback;
}>();
const emit = defineEmits<{
    wireStart: [event: PointerEvent];
    handleContext: [event: MouseEvent];
    pick: [];
}>();
</script>

<template>
    <div class="control-row" :class="{ unavailable, 'with-knob': $slots.knob, 'with-secondary': $slots.secondary,
        'connection-row': connectionId !== undefined }"
        :data-direction="endpoint.direction" :data-port-kind="endpoint.kind" :data-connection-feedback="feedback"
        :data-port-drop-target="connectionId === undefined && endpoint.direction === 'input' ? '' : undefined">
        <div v-if="$slots.knob" class="port-knob" data-port-control><slot name="knob" /></div>
        <button type="button" tabindex="-1" class="port-handle" :class="{ connected, pending }" data-port-control
            :data-port-endpoint="connectionId === undefined ? JSON.stringify(endpoint) : undefined"
            :data-connection-end="connectionId !== undefined ? JSON.stringify({ connectionId, port: endpoint }) : undefined"
            :data-status-hint="hint" :aria-description="description"
            :data-status-shortcuts="connectionChooser ? 'Right-click connections' : undefined"
            :aria-haspopup="connectionChooser ? 'dialog' : undefined"
            :aria-disabled="unavailable" :aria-label="`${endpoint.direction} ${label}`"
            @pointerdown="emit('wireStart', $event)" @contextmenu.stop.prevent="emit('handleContext', $event)"
            @click="connectionId !== undefined && emit('pick')">
            <span class="port-circle" />
        </button>
        <span class="direct-port-name iv-graph-header-detail" data-port-control data-port-label :data-status-hint="hint"
            :data-status-shortcuts="connectionChooser ? 'Right-click connections' : undefined"
            @contextmenu.stop.prevent="emit('handleContext', $event)"
            @pointerdown="connectionId !== undefined && emit('wireStart', $event)"
            @click="connectionId !== undefined && emit('pick')"><PortLabel :name="label" :short-name="shortLabel" /></span>
        <div v-if="$slots.secondary" class="secondary-control" data-port-control><slot name="secondary" /></div>
    </div>
</template>

<style scoped>
.control-row {
    --knob-size: var(--port-knob-size);
    --socket-tuck: var(--port-knob-center-tuck);
    --socket-control-column: var(--port-control-column);
    --socket-knob-edge-gap: max(0px, var(--port-knob-edge-gap));
    --handle-shift: calc(-1 * var(--socket-tuck) + (var(--port-socket-size) - var(--port-handle-width)) / 2);
    /* Keep the socket fixed even when the hit area shifts away from the knob. */
    --socket-shift: calc(-1 * var(--socket-tuck) - var(--handle-shift));
    display: grid; grid-template-columns: var(--socket-control-column) var(--port-handle-width) minmax(0, 1fr);
    grid-template-rows: var(--port-row-height);
    align-items: center; min-width: 0; flex-shrink: 0; --port-color: var(--iv-port-input); }
.control-row[data-direction="output"] { grid-template-columns: minmax(0, 1fr) var(--port-handle-width) var(--socket-control-column);
    --port-color: var(--iv-port-output); }
.control-row.unavailable { --port-color: var(--vscode-disabledForeground); color: var(--vscode-disabledForeground); }
.port-knob { display: flex; align-items: center; grid-column: 1; grid-row: 1; justify-self: end; margin-inline-end: var(--socket-knob-edge-gap); padding: 0; gap: 2px; }
.port-handle { position: relative; left: var(--handle-shift); grid-column: 2; grid-row: 1; display: grid;
    align-items: center; width: var(--port-handle-width); height: min(22px, var(--port-row-height)); overflow: visible;
    padding: 0; border: 0; border-radius: 5px; background: transparent; color: var(--port-color); cursor: crosshair; touch-action: none; }
[data-direction="output"] .port-knob { grid-column: 3; justify-self: start; margin-inline: var(--socket-knob-edge-gap) 0; }
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
    --socket-control-column: max(var(--port-control-column), var(--port-knob-size));
    --socket-tuck: calc(var(--knob-socket-tuck) + var(--socket-knob-edge-gap));
    --handle-shift: calc(-4px - var(--socket-knob-edge-gap));
    --cutout-center: calc(var(--knob-socket-tuck) - var(--knob-size) / 2);
    --cutout-radius: calc(var(--knob-size) * .3984375 + 1.5px);
}
.with-knob .port-knob { position: relative; z-index: 1; }
.port-knob > :deep(.manual-input) { padding: 0; gap: 0; }
.with-knob .port-handle::after { content: ""; position: absolute; inset: 0; z-index: 2; }
.with-knob .port-circle {
    --socket-mask: radial-gradient(circle at var(--cutout-center) 50%,
        transparent calc(var(--cutout-radius) - .5px), #000 calc(var(--cutout-radius) + .5px));
    -webkit-mask-image: var(--socket-mask);
    mask-image: var(--socket-mask);
}
[data-direction="output"] .port-handle { left: calc(-1 * var(--handle-shift)); }
[data-direction="output"] .port-circle {
    --cutout-center: calc(100% - var(--knob-socket-tuck) + var(--knob-size) / 2);
    justify-self: end;
    transform: translateX(calc(-1 * var(--socket-shift)));
}
.connected .port-circle { --port-circle-fill: currentColor; }
.pending .port-circle { border: 2px dashed currentColor; }
.control-row[data-connection-feedback] .port-circle { --port-circle-fill: var(--vscode-errorForeground); border: 0; }
.control-row[data-connection-feedback="edited"] .port-circle { outline: 1px solid var(--vscode-errorForeground); outline-offset: 2px; }
.port-handle[data-hovered] { background: color-mix(in srgb, currentColor 12%, transparent); }
.with-knob .port-handle[data-hovered] { background: transparent; }
.with-knob .port-handle[data-hovered] .port-circle { filter: brightness(1.25); }
.direct-port-name { grid-column: 3; grid-row: 1; padding-right: 3px; min-width: 0; overflow: hidden;
    justify-self: start; box-sizing: border-box; max-width: 100%; text-overflow: ellipsis; white-space: nowrap; font-size: 11px;
    color: color-mix(in srgb, var(--port-color) var(--iv-port-label-accent), var(--iv-port-label-base)); }
.unavailable .direct-port-name { color: var(--vscode-disabledForeground); }
[data-direction="output"] .direct-port-name { grid-column: 1; justify-self: end; text-align: right; }
.with-knob .direct-port-name { align-self: start; max-width: calc(100% + 17px);
    margin-block-start: 2px; margin-inline-start: -17px; line-height: 13px; }
[data-direction="output"].with-knob .direct-port-name {
    margin-inline-start: 0; margin-inline-end: -17px; padding-right: 0;
}
.secondary-control { grid-column: 4; grid-row: 1; display: flex; align-items: center; }
.control-row.with-secondary {
    grid-template-columns: var(--socket-control-column) var(--port-handle-width) minmax(0, 1fr) max-content;
}
.control-row.with-secondary[data-direction="output"] {
    grid-template-columns: max-content minmax(0, 1fr) var(--port-handle-width) var(--socket-control-column);
}
.with-secondary[data-direction="output"] .secondary-control { grid-column: 1; }
.with-secondary[data-direction="output"] .direct-port-name { grid-column: 2; }
.with-secondary[data-direction="output"] .port-handle { grid-column: 3; }
.with-secondary[data-direction="output"] .port-knob { grid-column: 4; }
.connection-row .direct-port-name { cursor: crosshair; touch-action: none; }
</style>

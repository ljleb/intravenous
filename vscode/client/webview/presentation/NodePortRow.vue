<script setup lang="ts">
import { computed } from "vue";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { ConnectionFeedback } from "../../src/graphScope";
import type { SemanticPort } from "./semanticNode";
import { portDescription, portDisplayName } from "./nodeSummary";
import ManualInput from "./ManualInput.vue";
import ConnectionDelayKnob from "./ConnectionDelayKnob.vue";
import PortSocketControl from "./PortSocketControl.vue";

const props = defineProps<{ port: SemanticPort; endpoint: GraphPortEndpoint;
    hasKnob: boolean; connected: boolean; pending: boolean; unavailable: boolean;
    hint: string; connectionChooser: boolean; feedback?: ConnectionFeedback;
    delay?: { end: GraphConnectionEnd; identity: string } }>();
const portName = computed(() => portDisplayName(props.port));
const emit = defineEmits<{
    wireStart: [event: PointerEvent]; handleContext: [event: MouseEvent];
}>();
</script>

<template>
    <PortSocketControl :endpoint="endpoint" :label="portName" :short-label="port.shortDisplayName"
        :description="portDescription(port)" :connected="connected" :pending="pending" :unavailable="unavailable"
        :hint="hint" :connection-chooser="connectionChooser" :feedback="feedback"
        @wire-start="emit('wireStart', $event)" @handle-context="emit('handleContext', $event)">
        <template v-if="delay || hasKnob" #knob>
            <ConnectionDelayKnob v-if="delay" class="connection-delay-knob" v-bind="delay" :port-name="portName" port-cutout />
            <ManualInput v-else :name="port.name" :direction="port.direction" port-cutout />
        </template>
        <template v-if="delay && hasKnob" #secondary>
            <ManualInput :name="port.name" :direction="port.direction" />
        </template>
    </PortSocketControl>
</template>

<script setup lang="ts">
defineProps<{ moduleKey: string }>();
</script>

<template>
    <div class="control-row implicit-output" data-direction="output">
        <span class="direct-port-name iv-graph-header-detail">Implicit nodes</span>
        <span class="implicit-socket" :data-implicit-output="moduleKey" aria-hidden="true">
            <span class="port-circle" />
        </span>
    </div>
</template>

<style scoped>
.implicit-output {
    display: grid; grid-template-columns: minmax(0, 1fr) var(--port-handle-width) var(--port-control-column);
    grid-template-rows: var(--port-row-height); align-items: center; min-width: 0; flex-shrink: 0;
    color: var(--vscode-descriptionForeground);
}
.direct-port-name {
    min-width: 0; padding-right: 3px; overflow: hidden; text-overflow: ellipsis;
    white-space: nowrap; text-align: right; font-size: 11px;
}
.implicit-socket { display: flex; justify-content: flex-end; }
.port-circle {
    width: var(--port-socket-size); height: var(--port-socket-size); border-radius: 50%;
    background: currentColor;
    transform: translateX(var(--socket-tuck));
}
</style>

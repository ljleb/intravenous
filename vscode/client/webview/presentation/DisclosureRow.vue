<script setup lang="ts">
import chevronDownSvg from "../../media/chevron_down.svg?raw";
import chevronLeftSvg from "../../media/chevron_left.svg?raw";

defineProps<{ expanded: boolean }>();
const currentColor = (icon: string) => icon.replace(/fill="[^"]*"/g, 'fill="currentColor"');
const collapsedIcon = currentColor(chevronLeftSvg);
const expandedIcon = currentColor(chevronDownSvg);
</script>

<template>
    <button type="button" tabindex="-1" class="disclosure-row" :aria-expanded="expanded">
        <span class="disclosure" :class="{ collapsed: !expanded }" aria-hidden="true"
            v-html="expanded ? expandedIcon : collapsedIcon" />
        <slot />
        <span v-if="$slots.detail" class="detail"><slot name="detail" /></span>
    </button>
</template>

<style scoped>
.disclosure-row {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    min-height: 22px;
    padding: 0 8px 0 var(--disclosure-indent, 24px);
    border: 0;
    background: transparent;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
}
.disclosure-row[data-hovered] { background: var(--vscode-list-hoverBackground); }
.disclosure { display: inline-flex; width: 12px; height: 12px; flex: 0 0 12px; color: var(--vscode-descriptionForeground); }
.disclosure :deep(svg) { width: 12px; height: 12px; }
/* Mirror the left-facing chevron when the row is collapsed. */
.disclosure.collapsed :deep(svg) { transform: scaleX(-1); }
.detail { flex: 0 1 auto; overflow: hidden; text-overflow: ellipsis; color: var(--vscode-descriptionForeground); }
</style>

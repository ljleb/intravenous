<template>
    <button type="button" tabindex="-1" class="context-menu-item">
        <span v-if="$slots.icon" class="menu-icon" aria-hidden="true"><slot name="icon" /></span>
        <slot />
    </button>
</template>

<style scoped>
.context-menu-item {
    display: flex; align-items: center; gap: 6px; width: 100%; min-width: 0; padding: 5px 8px;
    border: 0; border-radius: 2px; background: transparent; color: inherit; font: inherit;
    text-align: left; white-space: nowrap; cursor: pointer;
}
.context-menu-item:enabled:hover {
    color: var(--vscode-menu-selectionForeground, var(--vscode-list-activeSelectionForeground));
    background: var(--vscode-menu-selectionBackground, var(--vscode-list-activeSelectionBackground));
    box-shadow: inset 0 0 0 1px var(--vscode-menu-selectionBorder, transparent);
}
.context-menu-item:disabled { color: var(--vscode-disabledForeground); cursor: default; }
.menu-icon { flex: 0 0 12px; }
</style>

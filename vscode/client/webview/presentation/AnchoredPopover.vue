<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch, type CSSProperties } from "vue";
import type { Point } from "../../src/graphScope";
import type { GraphEditorHostMessage } from "../../src/graphEditorProtocol";
import { fitPopoverAtPoint } from "../../src/popoverPosition";
import { projectHistoryShortcut } from "../../src/projectHistoryShortcut";
import { useOptionalGraphEditor } from "../graph/context";

const props = defineProps<{ id?: string; label: string; width?: number; preferredHeight?: number;
    role?: "dialog" | "menu"; contextual?: boolean; dismissOnViewportChange?: boolean }>();
const emit = defineEmits<{ toggle: [open: boolean]; escape: [] }>();
const panel = ref<HTMLElement | null>(null);
const editor = useOptionalGraphEditor();
const position = ref<CSSProperties>({});
const open = ref(false);
let returnFocus: HTMLElement | SVGElement | null = null;
let atPoint: Point | null = null;
function place(anchor: HTMLElement) {
    atPoint = null;
    returnFocus = null;
    const rect = anchor.getBoundingClientRect();
    const below = window.innerHeight - rect.bottom, above = rect.top;
    const placeBelow = below >= (props.preferredHeight ?? 0) || below >= above;
    const width = Math.max(0, Math.min(props.width ?? rect.width, window.innerWidth - 16));
    const fitted = fitPopoverAtPoint({ x: rect.right - width, y: 0 }, { width, height: 0 }, viewport(), 8);
    position.value = {
        width: `${width}px`, left: `${fitted.x}px`,
        top: placeBelow ? `${rect.bottom + 4}px` : "auto",
        bottom: placeBelow ? "auto" : `${window.innerHeight - rect.top + 4}px`,
        maxHeight: `${Math.max(0, (placeBelow ? below : above) - 8)}px`,
    };
}
function viewport() { return { width: window.innerWidth, height: window.innerHeight }; }

function positionAtPoint() {
    const element = panel.value;
    if (!atPoint || !element?.matches(":popover-open")) return;
    const size = element.getBoundingClientRect();
    const point = fitPopoverAtPoint(atPoint, size, viewport());
    position.value = { left: `${point.x}px`, top: `${point.y}px` };
}
async function showAt(point: Point, focus: HTMLElement | SVGElement | null, initialFocus: string) {
    const element = panel.value;
    if (!element) return;
    atPoint = point;
    returnFocus = focus;
    if (!element.matches(":popover-open")) element.showPopover();
    positionAtPoint();
    await nextTick();
    if (atPoint === point && element.matches(":popover-open")) {
        element.querySelector<HTMLElement>(initialFocus)?.focus({ preventScroll: true });
    }
}
function close(restoreFocus = false) {
    const focus = returnFocus;
    returnFocus = null;
    atPoint = null;
    if (panel.value?.matches(":popover-open")) panel.value.hidePopover();
    if (restoreFocus && focus?.isConnected) focus.focus({ preventScroll: true });
}
function toggled(event: ToggleEvent) {
    open.value = event.newState === "open";
    if (!open.value) {
        if (panel.value) editor?.closeKnobValueWithin(panel.value);
        atPoint = null;
        returnFocus = null;
    }
    emit("toggle", open.value);
}
watch(open, (visible, _, cleanup) => {
    if (!visible) return;
    const listeners = new AbortController();
    const { signal } = listeners;
    cleanup(() => listeners.abort());
    const dismissOutside = (event: Event) => {
        if (event.target instanceof Node && panel.value?.contains(event.target)) return;
        close();
    };
    // A right-click can open a menu before that gesture's pointerup. Use the
    // next outside press to dismiss it, independent of the opening release.
    if (props.contextual) window.addEventListener("pointerdown", dismissOutside, { capture: true, signal });
    if (props.dismissOnViewportChange) {
        if (editor) {
            // Webview focus can briefly move during native activation.
            window.addEventListener("message", (event: MessageEvent<GraphEditorHostMessage>) => {
                if (event.data?.type === "graphWindowFocus" && !event.data.focused) close();
            }, { signal });
            document.addEventListener("visibilitychange", () => { if (document.hidden) close(); }, { signal });
        } else window.addEventListener("blur", dismissOutside, { signal });
        window.addEventListener("resize", dismissOutside, { signal });
        window.addEventListener("scroll", dismissOutside, { capture: true, signal });
    } else if (atPoint && panel.value) {
        const resize = new ResizeObserver(positionAtPoint);
        resize.observe(panel.value);
        window.addEventListener("resize", positionAtPoint);
        cleanup(() => { resize.disconnect(); window.removeEventListener("resize", positionAtPoint); });
    }
});
function keyDown(event: KeyboardEvent) {
    if (projectHistoryShortcut(event, false)) return;
    event.stopPropagation();
    if (event.key === "Escape") {
        event.preventDefault();
        emit("escape");
        close(true);
    }
}
onBeforeUnmount(() => { if (panel.value) editor?.closeKnobValueWithin(panel.value); });
defineExpose({ place, showAt, close });
</script>

<template>
    <div :id="id" ref="panel" class="anchored-popover" tabindex="-1" :class="{ 'context-menu': contextual }"
        :popover="contextual ? 'manual' : 'auto'" :style="position" :role="role ?? 'dialog'" :aria-label="label"
        @toggle="toggled" @keydown="keyDown" @pointerdown.stop @wheel.stop @contextmenu="contextual && $event.preventDefault()">
        <slot />
    </div>
</template>

<style scoped>
.anchored-popover {
    position: fixed; inset: auto; margin: 0; box-sizing: border-box; overflow: auto; padding: 10px;
    max-width: calc(100vw - 8px); max-height: calc(100vh - 8px);
    color: var(--vscode-editorWidget-foreground); background: var(--vscode-editorWidget-background);
    border: 1px solid var(--vscode-widget-border, var(--vscode-panel-border)); box-shadow: 0 2px 8px var(--vscode-widget-shadow);
}
.anchored-popover:popover-open { display: grid; gap: 8px; }
.context-menu:popover-open {
    min-width: min(160px, calc(100vw - 8px)); padding: 4px; border-radius: 4px; gap: 0;
    border-color: var(--vscode-menu-border, var(--vscode-widget-border, var(--vscode-panel-border)));
    background: var(--vscode-menu-background, var(--vscode-editorWidget-background));
    color: var(--vscode-menu-foreground, var(--vscode-editorWidget-foreground));
}
.context-menu :slotted(.context-menu-separator) {
    height: 0; margin: 4px 8px; border: 0;
    border-top: 1px solid var(--vscode-menu-separatorBackground, var(--vscode-panel-border));
}
</style>

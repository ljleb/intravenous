import { computed, nextTick, shallowRef } from "vue";
import type { Point } from "../../src/graphScope";

type FocusTarget = HTMLElement | SVGElement | null;
type ContextPopoverPanel = {
    showAt(point: Point, returnFocus: FocusTarget, initialFocus: string): Promise<void>;
    close(restoreFocus?: boolean): void;
};

export function isContextMenuKey(event: KeyboardEvent): boolean {
    return event.key === "ContextMenu" || (event.shiftKey && event.key === "F10");
}

export function useContextPopover<Key extends string | number>() {
    const panel = shallowRef<ContextPopoverPanel | null>(null);
    const selection = shallowRef<{ key: Key } | null>(null);
    const key = computed(() => selection.value?.key ?? null);

    function open(event: MouseEvent | KeyboardEvent, key: Key, options: {
        anchor: Element | null;
        alignRight?: boolean;
        returnFocus: FocusTarget;
        initialFocus: string;
    }): void {
        event.preventDefault();
        event.stopPropagation();
        const rect = options.anchor?.getBoundingClientRect();
        const point = "clientX" in event ? { x: event.clientX, y: event.clientY }
            : { x: (options.alignRight ? rect?.right : rect?.left) ?? 0, y: rect?.bottom ?? 0 };
        const opened = { key };
        selection.value = opened;
        void nextTick(() => {
            if (selection.value === opened) void panel.value?.showAt(point, options.returnFocus, options.initialFocus);
        });
    }
    function close(restoreFocus = false): void {
        panel.value?.close(restoreFocus);
        selection.value = null;
    }
    function toggled(open: boolean): void {
        if (!open) selection.value = null;
    }
    return { panel, key, open, close, toggled };
}

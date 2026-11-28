export function graphEditorShortcut(event: {
    key: string; ctrlKey: boolean; metaKey: boolean; altKey: boolean; shiftKey: boolean;
    isComposing?: boolean; defaultPrevented?: boolean;
}): "addNode" | "duplicateLanes" | "selectAll" | "copyKnobValue" | "pasteKnobValue" | "resetGraphState" | null {
    if (event.defaultPrevented || event.isComposing || event.altKey) return null;
    const key = event.key.toLowerCase(), command = event.ctrlKey || event.metaKey;
    if (command && !event.shiftKey) {
        if (key === "d") return "duplicateLanes";
        if (key === "a") return "selectAll";
    }
    if (!command && !event.shiftKey) {
        if (key === "c") return "copyKnobValue";
        if (key === "v") return "pasteKnobValue";
    }
    if (!command && event.shiftKey) {
        if (key === "a") return "addNode";
        if (key === "r") return "resetGraphState";
    }
    return null;
}

export function isTextEditingTarget(target: EventTarget | null): boolean {
    return target instanceof Element && !!target.closest(
        "input, textarea, select, [contenteditable]:not([contenteditable=false]), [role=textbox]",
    );
}

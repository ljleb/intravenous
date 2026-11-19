export function graphEditorShortcut(event: {
    key: string; ctrlKey: boolean; metaKey: boolean; altKey: boolean; shiftKey: boolean;
    isComposing?: boolean; defaultPrevented?: boolean;
}): "addNode" | "duplicateLanes" | "selectAll" | null {
    if (event.defaultPrevented || event.isComposing || event.altKey) return null;
    const key = event.key.toLowerCase(), command = event.ctrlKey || event.metaKey;
    if (command && !event.shiftKey) {
        if (key === "d") return "duplicateLanes";
        if (key === "a") return "selectAll";
    }
    return !command && event.shiftKey && key === "a" ? "addNode" : null;
}

export function isTextEditingTarget(target: EventTarget | null): boolean {
    return target instanceof Element && !!target.closest(
        "input, textarea, select, [contenteditable]:not([contenteditable=false]), [role=textbox]",
    );
}

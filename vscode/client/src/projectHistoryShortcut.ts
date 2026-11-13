// Runs only inside the graph webview; text editors retain their native history.
export function projectHistoryShortcut(event: {
    key: string; ctrlKey: boolean; metaKey: boolean; altKey: boolean; shiftKey: boolean;
    isComposing?: boolean; defaultPrevented?: boolean;
}, editingText: boolean): "undo" | "redo" | null {
    if (editingText || event.defaultPrevented || event.isComposing || event.altKey || !(event.ctrlKey || event.metaKey)) return null;
    const key = event.key.toLowerCase();
    if (key === "z") return event.shiftKey ? "redo" : "undo";
    if (key === "y" && event.ctrlKey && !event.metaKey && !event.shiftKey) return "redo";
    return null;
}

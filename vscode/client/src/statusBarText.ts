export const rightClickHint = "🖱︎R";
export const playbackShortcut = "Space ▶/Ⅱ";

export type StatusHint = { text?: string; shortcuts?: string };

export function isStatusHint(value: unknown): value is StatusHint {
    if (!value || typeof value !== "object") return false;
    const { text, shortcuts } = value as StatusHint;
    return (text === undefined || typeof text === "string") && (shortcuts === undefined || typeof shortcuts === "string");
}

export function formatStatusHint(hint: StatusHint, hideShortcuts: boolean): string {
    return statusBarText([hint.text, hideShortcuts ? undefined : hint.shortcuts].filter(Boolean).join(" · "));
}

export function statusBarText(text: string): string {
    return text.replace(/\s+/g, " ").trim().replace(/\$\(/g, "＄(");
}

export const rightClickHint = "🖱︎R";

export function statusBarText(text: string): string {
    return text.replace(/\s+/g, " ").trim().replace(/\$\(/g, "＄(");
}

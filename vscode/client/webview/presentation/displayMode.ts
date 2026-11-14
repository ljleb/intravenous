// The four global display modes of node_presentation_and_manual_controls_direction.md.
// A presentation receives exactly one of them.
export type NodeDisplayMode = "compact" | "nano" | "mini" | "editable";
export const nodeDisplayModes: readonly NodeDisplayMode[] = [
    "compact", "nano", "mini", "editable"];

// The modes a user can choose. `compact` is not a choice: the host imposes it
// whenever the available width is below the normal presentation width.
export type NodeViewChoice = "nano" | "mini" | "editable";
export const nodeViewChoices: readonly NodeViewChoice[] = ["nano", "mini", "editable"];

// Provisional Vue layout tuning values, not protocol guarantees.
export const compactWidthPx = 180;
const compactHysteresisPx = 16;
// Initial geometry thresholds, to tune against real presentations.
export const nanoHeightPx = 64;
export const editableHeightPx = 200;
const heightHysteresisPx = 12;

export function isNodeViewChoice(value: unknown): value is NodeViewChoice {
    return typeof value === "string"
        && (nodeViewChoices as readonly string[]).includes(value);
}

// Width selects compact first. Otherwise an explicit view choice wins;
// without one, allocated height selects nano, mini, or editable. Threshold
// hysteresis prevents small geometry changes from repeatedly switching modes.
export function effectiveDisplayMode(
    availableWidthPx: number | null,
    viewChoice: NodeViewChoice | null,
    previous: NodeDisplayMode | null,
    availableHeightPx: number | null = null,
): NodeDisplayMode {
    const threshold = previous === "compact"
        ? compactWidthPx + compactHysteresisPx
        : compactWidthPx;
    if (availableWidthPx !== null && availableWidthPx < threshold) return "compact";
    if (viewChoice) return viewChoice;
    if (availableHeightPx === null) return "mini";
    const nanoBoundary = previous === "nano" ? nanoHeightPx + heightHysteresisPx : nanoHeightPx;
    if (availableHeightPx <= nanoBoundary) return "nano";
    const editableBoundary = previous === "mini" ? editableHeightPx + heightHysteresisPx : editableHeightPx;
    return availableHeightPx < editableBoundary ? "mini" : "editable";
}

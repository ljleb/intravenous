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

export function isNodeViewChoice(value: unknown): value is NodeViewChoice {
    return typeof value === "string"
        && (nodeViewChoices as readonly string[]).includes(value);
}

// Chooses the one mode a presentation receives. Leaving `compact` requires
// slightly more width than entering it, so a width near the threshold does
// not alternate between modes. A width that is not measured yet counts as
// wide enough.
export function effectiveDisplayMode(
    availableWidthPx: number | null,
    viewChoice: NodeViewChoice,
    previous: NodeDisplayMode | null,
): NodeDisplayMode {
    if (availableWidthPx === null) {
        return viewChoice;
    }
    const threshold = previous === "compact"
        ? compactWidthPx + compactHysteresisPx
        : compactWidthPx;
    return availableWidthPx < threshold ? "compact" : viewChoice;
}

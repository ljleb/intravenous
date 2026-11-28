import type { SymbolicNode } from "../src/graphModel";
import type { LiveGraphInstance } from "../src/liveGraphProtocol";

// The nodes of the selected instance, or every node when no instance is
// selected or the selected instance has no node at the current source
// selection.
export function visibleNodes(
    nodes: readonly SymbolicNode[],
    selectedInstanceId: string | null,
): SymbolicNode[] {
    if (!selectedInstanceId) {
        return [...nodes];
    }
    const selected = nodes.filter((node) => node.instanceId === selectedInstanceId);
    return selected.length > 0 ? selected : [...nodes];
}

export function instanceLabel(instance: LiveGraphInstance): string {
    const label = instance.displayName
        || (instance.moduleId
            ? `${instance.moduleId} • ${instance.instanceId}`
            : instance.instanceId);
    return instance.realized ? label : `${label} [loading]`;
}

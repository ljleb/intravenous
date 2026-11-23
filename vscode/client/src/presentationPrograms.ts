import type { PresentationArtifact } from "./presentationPlugin";

export const genericPresentationId = "iv.generic";

export function projectProgramsByDefinition(plugins: readonly PresentationArtifact[]): Map<string, PresentationArtifact> {
    const selections = new Map<string, PresentationArtifact>();
    const isShipped = (plugin: PresentationArtifact) => plugin.id.startsWith("builtin/");
    for (const plugin of [...plugins].sort((a, b) => Number(isShipped(a)) - Number(isShipped(b)) || a.id.localeCompare(b.id))) {
        for (const id of plugin.manifest.definitionIds)
            if (!selections.has(id)) selections.set(id, plugin);
    }
    return selections;
}

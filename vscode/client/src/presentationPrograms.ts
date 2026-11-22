import { automationCurvePresentationManifest, createAutomationCurvePresentation } from "./automationCurvePresentation";
import { beatsPresentationManifest, createBeatsPresentation } from "./beatsPresentationPlugin";
import { clickDetectorPresentationManifest, createClickDetectorPresentation } from "./clickDetectorPresentation";
import type { PresentationArtifact, PresentationProgram } from "./presentationPlugin";

export const genericPresentationId = "iv.generic";

export const builtinPresentationPrograms = new Map<string, PresentationProgram>([
    ["builtin.beats", { manifest: beatsPresentationManifest, create: createBeatsPresentation }],
    ["builtin.click_detector", { manifest: clickDetectorPresentationManifest, create: createClickDetectorPresentation }],
    ["builtin.automation.curve", { manifest: automationCurvePresentationManifest, create: createAutomationCurvePresentation }],
]);

export function projectProgramsByDefinition(plugins: readonly PresentationArtifact[]): Map<string, PresentationArtifact> {
    const selections = new Map<string, PresentationArtifact>();
    for (const plugin of [...plugins].sort((a, b) => a.id.localeCompare(b.id))) {
        for (const id of plugin.manifest.definitionIds)
            if (!selections.has(id)) selections.set(id, plugin);
    }
    return selections;
}

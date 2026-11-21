import { automationCurvePresentationManifest, createAutomationCurvePresentation } from "./automationCurvePresentation";
import { beatsPresentationManifest, createBeatsPresentation } from "./beatsPresentationPlugin";
import { clickDetectorPresentationManifest, createClickDetectorPresentation } from "./clickDetectorPresentation";
import type { PresentationProgram, PresentationScript } from "./presentationPlugin";

export const genericPresentationId = "iv.generic";

export const builtinPresentationPrograms = new Map<string, PresentationProgram>([
    ["builtin.beats", { manifest: beatsPresentationManifest, create: createBeatsPresentation }],
    ["builtin.click_detector", { manifest: clickDetectorPresentationManifest, create: createClickDetectorPresentation }],
    ["builtin.automation.curve", { manifest: automationCurvePresentationManifest, create: createAutomationCurvePresentation }],
]);

export function projectProgramsByDefinition(plugins: readonly PresentationScript[]): Map<string, PresentationScript> {
    const selections = new Map<string, PresentationScript>();
    for (const plugin of [...plugins].sort((a, b) => a.manifest.id.localeCompare(b.manifest.id))) {
        if (plugin.manifest.id.startsWith("builtin.") || plugin.manifest.id === genericPresentationId) continue;
        for (const id of plugin.manifest.definitionIds)
            if (!selections.has(id)) selections.set(id, plugin);
    }
    return selections;
}

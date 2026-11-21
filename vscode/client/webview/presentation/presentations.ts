import { markRaw, shallowRef, type Component } from "vue";

import GenericPresentation from "./GenericPresentation.vue";
import PluginLane from "./PluginLane.vue";
import { PresentationRegistry } from "./registry";
import { beatsPresentationManifest, createBeatsPresentation } from "../../src/beatsPresentationPlugin";
import { automationCurvePresentationManifest, createAutomationCurvePresentation } from "../../src/automationCurvePresentation";
import type { PresentationProgram, PresentationScript } from "../../src/presentationPlugin";

export const genericPresentationId = "iv.generic";

// The client's presentation registry. Specialized presentations bundled with
// the client register here by stable leaf or module definition ID.
const presentations = new PresentationRegistry<Component>({
    presentationId: genericPresentationId,
    displayName: "Generic",
    component: markRaw(GenericPresentation),
});
presentations.register({ presentationId: "builtin.timeline", displayName: "Timeline",
    definitionIds: ["builtin.timeline"], component: markRaw(GenericPresentation), timeline: true,
    icon: "M2 5h12v7H2zm3 0v3m3-3v4m3-4v3" });

const hosted = markRaw(PluginLane);
const builtinPrograms = new Map<string, PresentationProgram>([
    ["builtin.beats", { manifest: beatsPresentationManifest, create: createBeatsPresentation }],
    ["builtin.automation.curve", { manifest: automationCurvePresentationManifest, create: createAutomationCurvePresentation }],
]);
const projectPrograms = shallowRef(new Map<string, { program: PresentationScript; serial: number }>());
let nextSerial = 0;

export function replaceProjectPresentations(plugins: PresentationScript[]): void {
    const next = new Map<string, { program: PresentationScript; serial: number }>();
    const previous = projectPrograms.value;
    for (const plugin of [...plugins].sort((a, b) => a.manifest.id.localeCompare(b.manifest.id))) {
        if (plugin.manifest.id.startsWith("builtin.") || plugin.manifest.id === genericPresentationId) continue;
        for (const id of plugin.manifest.definitionIds) {
            if (next.has(id)) continue;
            const existing = previous.get(id);
            next.set(id, existing && existing.program.source === plugin.source
                && JSON.stringify(existing.program.manifest) === JSON.stringify(plugin.manifest)
                ? existing : { program: plugin, serial: ++nextSerial });
        }
    }
    projectPrograms.value = next;
}

export function selectPresentation(definitionId: string) {
    const project = projectPrograms.value.get(definitionId);
    const program = project?.program ?? builtinPrograms.get(definitionId);
    if (program) {
        const manifest = program.manifest;
        const configuration = manifest.initialConfiguration;
        return { component: hosted, program, presentationId: manifest.id, displayName: manifest.title, usedGeneric: false,
            timeline: manifest.timeline, icon: manifest.icon, serial: project?.serial ?? 0,
            createInitialConfiguration: configuration === undefined ? undefined : () => configuration };
    }
    return { ...presentations.select(definitionId), program: undefined, serial: 0 };
}

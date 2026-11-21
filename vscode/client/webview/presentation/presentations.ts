import { markRaw, shallowRef, type Component } from "vue";

import GenericPresentation from "./GenericPresentation.vue";
import PluginLane from "./PluginLane.vue";
import { PresentationRegistry } from "./registry";
import { builtinPresentationPrograms, genericPresentationId, projectProgramsByDefinition } from "../../src/presentationPrograms";
import type { PresentationScript } from "../../src/presentationPlugin";
export { genericPresentationId };

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
const projectPrograms = shallowRef(new Map<string, { program: PresentationScript; serial: number }>());
let nextSerial = 0;

export function replaceProjectPresentations(plugins: PresentationScript[]): void {
    const next = new Map<string, { program: PresentationScript; serial: number }>();
    const previous = projectPrograms.value;
    for (const [id, plugin] of projectProgramsByDefinition(plugins)) {
        const existing = previous.get(id);
        next.set(id, existing && existing.program.source === plugin.source
            && JSON.stringify(existing.program.manifest) === JSON.stringify(plugin.manifest)
            ? existing : { program: plugin, serial: ++nextSerial });
    }
    projectPrograms.value = next;
}

export function selectPresentation(definitionId: string) {
    const project = projectPrograms.value.get(definitionId);
    const program = project?.program ?? builtinPresentationPrograms.get(definitionId);
    if (program) {
        const manifest = program.manifest;
        const configuration = manifest.initialConfiguration;
        return { component: hosted, program, presentationId: manifest.id, displayName: manifest.title, usedGeneric: false,
            timeline: manifest.timeline, icon: manifest.icon, serial: project?.serial ?? 0,
            createInitialConfiguration: configuration === undefined ? undefined : () => configuration };
    }
    return { ...presentations.select(definitionId), program: undefined, serial: 0 };
}

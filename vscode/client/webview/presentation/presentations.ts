import { markRaw, type Component } from "vue";

import GenericPresentation from "./GenericPresentation.vue";
import AutomationCurvePresentation from "./AutomationCurvePresentation.vue";
import BeatsPresentation from "./BeatsPresentation.vue";
import { PresentationRegistry } from "./registry";

export const genericPresentationId = "iv.generic";

// The client's presentation registry. Specialized presentations bundled with
// the client register here by stable leaf or module definition ID.
export const presentations = new PresentationRegistry<Component>({
    presentationId: genericPresentationId,
    displayName: "Generic",
    component: markRaw(GenericPresentation),
});
presentations.register({ presentationId: "builtin.automation.curve", displayName: "Automation curve",
    definitionIds: ["builtin.automation.curve"], component: markRaw(AutomationCurvePresentation), timeline: true,
    icon: "M2 12C5 12 5 4 14 4M2 10v4m12-12v4" });
presentations.register({ presentationId: "builtin.beats", displayName: "Beats",
    definitionIds: ["builtin.beats"], component: markRaw(BeatsPresentation), timeline: true,
    icon: "M2 3v10m4-7v7m4-4v4m4-7v7" });
presentations.register({ presentationId: "builtin.timeline", displayName: "Timeline",
    definitionIds: ["builtin.timeline"], component: markRaw(GenericPresentation), timeline: true,
    icon: "M2 5h12v7H2zm3 0v3m3-3v4m3-4v3" });

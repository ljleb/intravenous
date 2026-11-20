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
    definitionIds: ["builtin.automation.curve"], component: markRaw(AutomationCurvePresentation), timeline: true });
presentations.register({ presentationId: "builtin.beats", displayName: "Beats",
    definitionIds: ["builtin.beats"], component: markRaw(BeatsPresentation), timeline: true });
presentations.register({ presentationId: "builtin.timeline", displayName: "Timeline",
    definitionIds: ["builtin.timeline"], component: markRaw(GenericPresentation), timeline: true });

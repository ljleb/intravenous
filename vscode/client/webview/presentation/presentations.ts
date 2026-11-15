import { markRaw, type Component } from "vue";

import GenericPresentation from "./GenericPresentation.vue";
import AutomationCurvePresentation from "./AutomationCurvePresentation.vue";
import { PresentationRegistry } from "./registry";

export const genericPresentationId = "iv.generic";

// The client's presentation registry. Specialized presentations bundled with
// the client register here by stable leaf or module definition ID.
export const presentations = new PresentationRegistry<Component>({
    presentationId: genericPresentationId,
    displayName: "Generic",
    component: markRaw(GenericPresentation),
});
presentations.register({ presentationId: "iv.builtin.automation.curve", displayName: "Automation curve",
    definitionIds: ["iv.builtin.automation.curve"], component: markRaw(AutomationCurvePresentation),
    // An empty point list; points are added and edited on the curve.
    initialConfiguration: "{}" });

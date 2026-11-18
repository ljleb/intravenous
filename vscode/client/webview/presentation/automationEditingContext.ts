import { inject, type InjectionKey, type Ref } from "vue";
import type { AutomationCurveEditing } from "../../src/automationCurveEditing";

export type AutomationEditingContext = {
    edits: AutomationCurveEditing;
    version: Ref<number>;
};
export const automationEditingKey: InjectionKey<AutomationEditingContext> = Symbol("iv.automationEditing");
export function useAutomationEditing(): AutomationEditingContext {
    const context = inject(automationEditingKey, null);
    if (!context) throw new Error("Automation presentation requires its webview editing owner");
    return context;
}

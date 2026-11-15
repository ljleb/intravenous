import { inject, type InjectionKey, type Ref } from "vue";
import type { AutomationCurveEditing, CurveAxes } from "../../src/automationCurveEditing";
import type { NodeAddress } from "../../src/graphScope";

export type AutomationEditingContext = {
    edits: AutomationCurveEditing;
    version: Ref<number>;
    begin(event: PointerEvent, target: NodeAddress, pointId: number, axes: CurveAxes): void;
};
export const automationEditingKey: InjectionKey<AutomationEditingContext> = Symbol("iv.automationEditing");
export function useAutomationEditing(): AutomationEditingContext {
    const context = inject(automationEditingKey, null);
    if (!context) throw new Error("Automation presentation requires its webview editing owner");
    return context;
}

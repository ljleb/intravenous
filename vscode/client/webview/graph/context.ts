import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { GraphLayout, NodeAddress } from "../../src/graphScope";

type GraphEditorContext = {
    layout: Ref<GraphLayout>;
    save: () => void;
    copyNodeDebugDetails: (address: NodeAddress) => void;
};
const key: InjectionKey<GraphEditorContext> = Symbol("graphEditor");
export function provideGraphEditor(context: GraphEditorContext) { provide(key, context); }
export function useGraphEditor(): GraphEditorContext {
    const context = inject(key);
    if (!context) throw new Error("Graph presentation needs a graph editor host");
    return context;
}

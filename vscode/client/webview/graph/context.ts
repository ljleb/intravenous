import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { GraphLayout, NodeAddress } from "../../src/graphScope";
import type { GraphScope } from "../../src/graphScope";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { PortConnectionDrag } from "../../src/portConnectionGesture";

type GraphEditorContext = {
    layout: Ref<GraphLayout>;
    save: () => void;
    copyNodeDebugDetails: (address: NodeAddress) => void;
    scope: Readonly<Ref<GraphScope | null>>;
    selectedLane: Readonly<Ref<string | null>>;
    selectLane: (address: NodeAddress) => void;
    portsChanged: () => void;
    connectionDrag: Ref<PortConnectionDrag | null>;
    hoveredPortGroup: Ref<{ nodeKey: string; direction: "input" | "output" } | null>;
    startConnection: (event: PointerEvent, port: GraphPortEndpoint, edit?: GraphConnectionEnd) => void;
    cancelConnection: () => void;
};
const key: InjectionKey<GraphEditorContext> = Symbol("graphEditor");
export function provideGraphEditor(context: GraphEditorContext) { provide(key, context); }
export function useOptionalGraphEditor(): GraphEditorContext | null { return inject(key, null); }
export function useGraphEditor(): GraphEditorContext {
    const context = inject(key);
    if (!context) throw new Error("Graph presentation needs a graph editor host");
    return context;
}

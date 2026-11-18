import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { GraphLayout, NodeAddress, Point } from "../../src/graphScope";
import type { GraphScope } from "../../src/graphScope";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { PortConnectionDrag } from "../../src/portConnectionGesture";
import type { CurveAxes, CurveDragKind } from "../../src/automationCurveEditing";

export type CursorWarpAnchor = { screenOrigin: Point; scale: number };

type GraphEditorContext = {
    layout: Ref<GraphLayout>;
    save: () => void;
    copyNodeDebugDetails: (address: NodeAddress) => void;
    scope: Readonly<Ref<GraphScope | null>>;
    shiftHeld: Readonly<Ref<boolean>>;
    selectedLanes: Readonly<Ref<ReadonlySet<string>>>;
    selectLane: (address: NodeAddress, modifiers?: { ctrlKey?: boolean; metaKey?: boolean; shiftKey?: boolean }) => void;
    portsChanged: () => void;
    elementAtPointer: () => Element | null;
    beginPointerLockExit: (origin: Point, warp?: CursorWarpAnchor & { destination: Point; trace?: boolean }) => void;
    takeMiddleCursorTrace: () => ((detail: string) => void) | null;
    reportCursorReturnSkipped: (detail: string) => void;
    setTimelineZoomHint: (hint: string | null) => void;
    connectionDrag: Ref<PortConnectionDrag | null>;
    hoveredPortGroup: Ref<{ nodeKey: string; direction: "input" | "output" } | null>;
    startConnection: (event: PointerEvent, port: GraphPortEndpoint, edit?: GraphConnectionEnd) => void;
    cancelConnection: () => void;
    startAutomation: (event: PointerEvent, target: NodeAddress, pointId: number | null, axes: CurveAxes, kind: CurveDragKind,
        visiblePointCenter?: (id: number) => Point | null) => void;
};
const key: InjectionKey<GraphEditorContext> = Symbol("graphEditor");
export function provideGraphEditor(context: GraphEditorContext) { provide(key, context); }
export function useOptionalGraphEditor(): GraphEditorContext | null { return inject(key, null); }
export function useGraphEditor(): GraphEditorContext {
    const context = inject(key);
    if (!context) throw new Error("Graph presentation needs a graph editor host");
    return context;
}

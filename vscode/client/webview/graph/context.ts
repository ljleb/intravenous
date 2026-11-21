import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { GraphLayout, NodeAddress, Point, PresentationState } from "../../src/graphScope";
import type { GraphScope } from "../../src/graphScope";
import type { GraphLane } from "../../src/laneSelection";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { PortConnectionDrag } from "../../src/portConnectionGesture";
import type { CurveAxes, CurveDragKind } from "../../src/automationCurveEditing";
import type { KnobLimits } from "../../src/knobScrub";
import type { StatusHint } from "../../src/statusBarText";
import type { CursorFollow, CursorFollowTarget, CursorTrace } from "./cursorReturn";

export type ObjectDrag = {
    trace?: CursorTrace;
    move: (point: Point) => void;
    moveBy: (delta: Point) => void;
    modifiers?: (event: MouseEvent | KeyboardEvent) => void;
    finish: (point: Point | null, lockOrigin: Point | null) => CursorFollowTarget | undefined;
};
export type KnobDrag = {
    move: (point: Point) => void;
    moveBy: (delta: Point) => void;
    changeSensitivity: (delta: number) => boolean;
    finish: (point: Point | null) => void;
    active?: () => boolean;
    hint?: () => StatusHint;
};
export type KnobClipboard = {
    identity: () => string;
    value: () => number | undefined;
    paste: (text: string) => string | undefined;
};

type GraphEditorContext = {
    layout: Ref<GraphLayout>;
    save: () => void;
    readPresentationState: (key: string) => PresentationState | undefined;
    presentationStateChanged: (key: string, serialize: () => PresentationState | null) => void;
    copyNodeDebugDetails: (address: NodeAddress) => void;
    scope: Readonly<Ref<GraphScope | null>>;
    lanes: Readonly<Ref<GraphLane[]>>;
    expandedModules: Readonly<Ref<ReadonlyMap<string, NodeAddress>>>;
    toggleModule: (address: NodeAddress) => void;
    shiftHeld: Readonly<Ref<boolean>>;
    selectedLanes: Readonly<Ref<ReadonlySet<string>>>;
    portsChanged: () => void;
    profileGraphUi: <T>(label: string, run: () => T) => T;
    elementAtPointer: () => Element | null;
    takePlayheadTrace: () => CursorTrace | undefined;
    startObjectDrag: (event: PointerEvent, drag: ObjectDrag) => (() => void) | undefined;
    finishPointerLock: (release: Point | null, lockOrigin: Point | null, follow?: CursorFollow, lockError?: string) => void;
    setTimelineZoomHint: (hint: string | null) => void;
    connectionDrag: Ref<PortConnectionDrag | null>;
    hoveredPortGroup: Ref<{ nodeKey: string; rowKey?: string; direction: "input" | "output" } | null>;
    startConnection: (event: PointerEvent, port: GraphPortEndpoint, edit?: GraphConnectionEnd) => void;
    cancelConnection: () => void;
    activeKnob: Readonly<Ref<string | null>>;
    startKnob: (event: PointerEvent, key: string, begin: () => KnobDrag | undefined, reset?: () => void) => void;
    cancelKnob: () => boolean;
    registerKnobClipboard: (element: HTMLElement, knob: KnobClipboard) => () => void;
    openKnobValue: (anchor: HTMLElement, label: string, value: number, limits: () => KnobLimits,
        apply: (value: number) => void, bindValueEntry?: (anchor: DOMRect) => void) => void;
    closeKnobValueWithin: (panel: HTMLElement) => void;
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

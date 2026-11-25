import { inject, provide, type InjectionKey, type Ref } from "vue";
import type { GraphLayout, NodeAddress, Point } from "../../src/graphScope";
import type { SerializedPresentationState } from "../../src/presentationStateJson";
import type { PresentationStateRead } from "../../src/laneViewReads";
import type { GraphScope } from "../../src/graphScope";
import type { laneConnectionFeedback } from "../../src/selectedLaneConnections";
import type { GraphLane } from "../../src/laneSelection";
import type { GraphConnectionEnd, GraphPortEndpoint } from "../../src/graphConnectionEditing";
import type { PortConnectionDrag } from "../../src/portConnectionGesture";
import type { KnobLimits } from "../../src/knobScrub";
import type { StatusHint } from "../../src/statusBarText";
import type { CursorReturnPolicy } from "../../src/graphEditorProtocol";
import type { CursorFollow, CursorFollowTarget, CursorTrace } from "./cursorReturn";

export type ObjectDrag = {
    trace?: CursorTrace;
    cursorReturnPolicy?: CursorReturnPolicy;
    lock?: false;
    hideCursor?: boolean;
    begin?: () => boolean;
    wheel?: (delta: number) => boolean;
    hint?: () => StatusHint | undefined;
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
    requestPresentationState: (key: string) => PresentationStateRead;
    presentationStateChanged: (key: string, state: SerializedPresentationState) => void;
    reportPresentationError: (details: string) => void;
    copyNodeDebugDetails: (address: NodeAddress) => void;
    scope: Readonly<Ref<GraphScope | null>>;
    lanes: Readonly<Ref<GraphLane[]>>;
    expandedModules: Readonly<Ref<ReadonlyMap<string, NodeAddress>>>;
    implicitOutputModules: Readonly<Ref<ReadonlySet<string>>>;
    connectionFeedback: Readonly<Ref<ReturnType<typeof laneConnectionFeedback>>>;
    toggleModule: (address: NodeAddress) => void;
    shiftHeld: Readonly<Ref<boolean>>;
    selectedLanes: Readonly<Ref<ReadonlySet<string>>>;
    portsChanged: () => void;
    profileGraphUi: <T>(label: string, run: () => T) => T;
    elementAtPointer: () => Element | null;
    takeCursorReturnTrace: () => CursorTrace | undefined;
    startObjectDrag: (event: PointerEvent, drag: ObjectDrag) => ((reason?: string) => void) | undefined;
    refreshPointerHint: () => void;
    historyPending: Readonly<Ref<boolean>>;
    registerPresentationInterrupt: (interrupt: (reason: "history" | "disconnected") => void) => () => void;
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
};
const key: InjectionKey<GraphEditorContext> = Symbol("graphEditor");
export function provideGraphEditor(context: GraphEditorContext) { provide(key, context); }
export function useOptionalGraphEditor(): GraphEditorContext | null { return inject(key, null); }
export function useGraphEditor(): GraphEditorContext {
    const context = inject(key);
    if (!context) throw new Error("Graph presentation needs a graph editor host");
    return context;
}

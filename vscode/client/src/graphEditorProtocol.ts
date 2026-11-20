import type { ObservationHostRequest, ObservationResponse } from "./observations";
import type { ObservationDelivery } from "./observationDelivery";
import type { ObservationProfileSummary } from "./observationProfile";
import type { ManualControlAction, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import type { GraphLaneCopy, GraphScope, NodeAddress, PresentationState } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionEdit, GraphPortEndpoint } from "./graphConnectionEditing";
import type { NodeEventHostRequest, NodeEventResponse } from "./nodeEventMessaging";
import type { PlaybackDragPhase, PlaybackSnapshot } from "./playback";
import type { StatusHint } from "./statusBarText";
import type { TimelineGridMode } from "./timelineSnap";
import type { NodeDefinitionCatalog, NodeDefinitionSearch } from "./nodeDefinitionSearch";

export type CursorWarp = {
    originX: number; originY: number;
    screenOrigin?: { x: number; y: number };
    deltaX: number; deltaY: number;
    expiresAt: number;
};
export type CursorReturnPolicy = "automationPoint" | "playhead" | "wrapped";
export type TraceDetails = Record<string, unknown>;
export type TraceLog = (step: string, details?: TraceDetails) => void;
export type CursorReturnMessage =
    | { type: "returnCursor"; warp: CursorWarp; requestId: number; policy?: CursorReturnPolicy; traceId?: string }
    | { type: "cursorReturnSkipped"; detail: string };

export type GraphPresentationPatch = {
    presentationStates: Record<string, PresentationState | null>;
    laneOrder?: string[];
};

export type GraphEditorState = {
    sessionStatus: LiveGraphSessionStatus;
    scope: GraphScope | null;
    definitions: NodeDefinitionCatalog | null;
    definitionUsage: Readonly<Record<string, number>>;
    error: string;
};
export type GraphEditorAction = "addNode" | "editNode" | "deleteNode" | "openNodeSource" | "createPackage" | "deleteConnection" | "undo" | "redo";
export type GraphLaneInsertion = { instanceId: string };
export type GraphEditorHostMessage = { type: "graphState"; state: GraphEditorState }
    | { type: "playbackUpdate"; snapshot: PlaybackSnapshot | null; requestedPosition: string | null;
        releasedDragPosition: string | null; dragReleaseId?: string }
    | ObservationResponse
    | { type: "observationVisibility"; visible: boolean }
    | { type: "observationUpdate"; update: ObservationDelivery; sequence: number; previousSequence?: number }
    | { type: "startObservationProfile"; durationMs: number }
    | { type: "startPlayheadTrace"; traceId: string }
    | NodeEventResponse
    | { type: "manualControlState"; interactions: ManualControlInteractionState[]; live: ManualControlLiveValue[] }
    | { type: "graphWindowFocus"; focused: boolean }
    | { type: "cursorReturnCompleted"; requestId: number }
    | { type: "knobClipboardResult"; requestId: number; text?: string; error?: string }
    | { type: "graphLanesInserted"; lanes: GraphLaneInsertion[]; duplicateRequestId?: string }
    | { type: "graphPresentationAccepted"; requestId: number; revision?: number }
    | { type: "nodeDefinitionSearchResult"; requestId: string; result?: NodeDefinitionSearch; error?: string }
    | { type: "graphAction"; action: "addNode" | "editNode" | "createPackage"; instanceId?: string };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "setTimelineSnapMode"; mode: TimelineGridMode }
    | { type: "playbackToggle" }
    | { type: "playbackSeek"; position: string }
    | { type: "playbackDrag"; phase: PlaybackDragPhase; position: string; releaseId?: string }
    | ObservationHostRequest
    | { type: "observationResync" }
    | { type: "observationProfile"; summary: ObservationProfileSummary }
    | { type: "playheadTrace"; traceId: string; step: string; details?: TraceDetails }
    | NodeEventHostRequest
    | { type: "graphInteractionHint"; hint: StatusHint | null }
    | CursorReturnMessage
    | { type: "manualControlAction"; action: ManualControlAction }
    | { type: "deleteSelectedLanes"; instanceIds: string[] }
    | { type: "duplicateSelectedLanes"; copies: GraphLaneCopy[]; requestId: string }
    | { type: "setManualInputEnabled"; target: NodeAddress; inputName: string; enabled: boolean }
    | { type: "projectHistory"; direction: "undo" | "redo" }
    | { type: "copyNodeDebugDetails"; nodeKey: string }
    | { type: "flushProjectAutosave" }
    | { type: "knobClipboard"; requestId: number; text?: string }
    | { type: "renameGraphLane"; instanceId: string; displayName: string; previousDisplayName: string }
    | { type: "connectGraphPorts"; first: GraphPortEndpoint; second: GraphPortEndpoint }
    | { type: "editGraphConnectionEnd"; edit: GraphConnectionEdit }
    | { type: "searchNodeDefinitions"; requestId: string; source: string; cursor: number }
    | { type: "createGraphNode"; definitionId: string; displayName: string; configurationSource: string; afterInstanceId?: string }
    | { type: "updateGraphNode"; instanceId: string; displayName: string; configurationSource: string;
        previousDisplayName: string; previousConfigurationSource: string }
    | { type: "createGraphPackage"; name: string }
    | ({ type: "saveGraphPresentation"; requestId: number } & GraphPresentationPatch);

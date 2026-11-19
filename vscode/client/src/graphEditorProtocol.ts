import type { ObservationHostRequest, ObservationResponse } from "./observations";
import type { ObservationDelivery } from "./observationDelivery";
import type { ObservationProfileSummary } from "./observationProfile";
import type { ManualControlAction, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import { readLayout, type GraphLaneCopy, type GraphLayout, type GraphScope, type GraphViewState, type NodeAddress, type PresentationState } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionEdit, GraphPortEndpoint } from "./graphConnectionEditing";
import type { NodeEventHostRequest, NodeEventResponse } from "./nodeEventMessaging";
import type { PlaybackDragPhase, PlaybackSnapshot } from "./playback";
import type { StatusHint } from "./statusBarText";
import type { TimelineGridMode } from "./timelineSnap";

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

type SaveGraphLayoutMessage = {
    type: "saveGraphLayout";
    layout: Omit<GraphLayout, keyof GraphViewState>;
    presentationStates: Record<string, PresentationState | null>;
    laneOrder?: string[];
};

export function saveGraphLayoutMessage(layout: GraphLayout, presentationStates: SaveGraphLayoutMessage["presentationStates"],
    laneOrderChanged: boolean): SaveGraphLayoutMessage {
    // Send only changed presentation states. Saved states can still contain
    // reactive objects that the webview message channel cannot clone.
    const { presentationStates: _storedStates, laneOrder, ...geometry } = readLayout(layout);
    return { type: "saveGraphLayout", layout: geometry, presentationStates, ...(laneOrderChanged ? { laneOrder } : {}) };
}

export type GraphEditorState = {
    sessionStatus: LiveGraphSessionStatus;
    scope: GraphScope | null;
    layout: GraphLayout;
    definitions: string[];
    error: string;
};
export type GraphEditorAction = "addNode" | "editNode" | "deleteNode" | "openNodeSource" | "createPackage" | "deleteConnection" | "undo" | "redo";
export type GraphLaneInsertion = { instanceId: string; afterInstanceId?: string };
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
    | { type: "graphLanesInserted"; lanes: GraphLaneInsertion[]; duplicateRequestId?: string }
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
    | { type: "renameGraphLane"; instanceId: string; displayName: string; previousDisplayName: string }
    | { type: "connectGraphPorts"; first: GraphPortEndpoint; second: GraphPortEndpoint }
    | { type: "editGraphConnectionEnd"; edit: GraphConnectionEdit }
    | { type: "createGraphNode"; definitionId: string; displayName: string; configurationSource: string; afterInstanceId?: string }
    | { type: "updateGraphNode"; instanceId: string; displayName: string; configurationSource: string;
        previousDisplayName: string; previousConfigurationSource: string }
    | { type: "createGraphPackage"; name: string }
    | SaveGraphLayoutMessage;

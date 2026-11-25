import type { ObservationHostRequest, ObservationResponse } from "./observations";
import type { ObservationDelivery } from "./observationDelivery";
import type { ObservationProfileSummary } from "./observationProfile";
import type { GraphUiProfileSummary } from "./graphUiProfile";
import type { ManualControlAction, ManualControlId, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import type { GraphLaneCopy, GraphScope, NodeAddress, ProjectEditReceipt } from "./graphScope";
import type { SerializedPresentationState } from "./presentationStateJson";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionChange, GraphPortEndpoint } from "./graphConnectionEditing";
import type { NodeEventHostRequest, NodeEventResponse } from "./nodeEventMessaging";
import type { PlaybackDragPhase, PlaybackSnapshot } from "./playback";
import type { StatusHint } from "./statusBarText";
import type { TimelineGridMode } from "./timelineSnap";
import type { NodeDefinitionCatalog, NodeDefinitionSearch } from "./nodeDefinitionSearch";
import type { PresentationArtifact } from "./presentationPlugin";
import type { InteractionRecording } from "./clientInteractionTrace";

export type CursorWindowDestination = {
    windowSizeDip: { width: number; height: number };
    windowPointDip: { x: number; y: number };
    offsetPixels: { x: number; y: number };
};
export type CursorWarp = { expiresAt: number } & ({ destination: CursorWindowDestination } | {
    originX: number; originY: number;
    screenOrigin?: { x: number; y: number };
    deltaX: number; deltaY: number;
});
export type CursorReturnPolicy = "presentationPoint" | "playhead" | "wrapped";
export type TraceDetails = Record<string, unknown>;
export type TraceLog = (step: string, details?: TraceDetails) => void;
export type CursorReturnMessage =
    | { type: "returnCursor"; warp: CursorWarp; requestId: number; policy?: CursorReturnPolicy; traceId?: string }
    | { type: "cursorReturnSkipped"; detail: string };

export type GraphPresentationPatch = {
    presentationStates: Record<string, SerializedPresentationState>;
    laneOrder?: string[];
    laneParents?: Record<string, string>;
};

export type GraphEditorState = {
    sessionStatus: LiveGraphSessionStatus;
    connectionNumber?: number;
    scope: GraphScope | null;
    definitions: NodeDefinitionCatalog | null;
    definitionUsage: Readonly<Record<string, number>>;
};
export type GraphEditorAction = "addNode" | "editNode" | "deleteNode" | "openNodeSource" | "createPackage" | "deleteConnection" | "undo" | "redo";
export type GraphLaneInsertion = { instanceId: string };
export type GraphEditorHostMessage = { type: "graphState"; state: GraphEditorState }
    | { type: "projectPresentations"; plugins: PresentationArtifact[] }
    | { type: "playbackUpdate"; snapshot: PlaybackSnapshot | null; requestedPosition: string | null;
        releasedDragPosition: string | null; dragReleaseId?: string }
    | ObservationResponse
    | { type: "graphVisibility"; visible: boolean }
    | { type: "observationUpdate"; update: ObservationDelivery; sequence: number; previousSequence?: number }
    | { type: "startObservationProfile"; durationMs: number }
    | { type: "startClientInteractionTrace"; traceId: string; durationMs: number }
    | { type: "startGraphUiProfile"; profileId: string; durationMs: number }
    | { type: "startPlaybackProfile"; profileId: string; durationMs: number }
    | { type: "startCursorReturnTrace"; traceId: string }
    | NodeEventResponse
    | { type: "manualControlState"; interactions: ManualControlInteractionState[]; live: ManualControlLiveValue[] }
    | { type: "graphWindowFocus"; focused: boolean }
    | { type: "cursorReturnCompleted"; requestId: number }
    | { type: "knobClipboardResult"; requestId: number; text?: string; error?: string }
    | { type: "graphLanesInserted"; lanes: GraphLaneInsertion[]; duplicateRequestId?: string }
    | { type: "graphPresentationAccepted"; requestId: number; receipt?: ProjectEditReceipt }
    | { type: "graphEditFinished"; requestId: number; accepted: boolean }
    | { type: "laneViewStates"; reads: Array<{ requestId: number; version: string; state: string | null }> }
    | { type: "interruptProjectHistory"; requestId: number }
    | { type: "projectHistoryFinished"; requestId: number }
    | { type: "nodeDefinitionSearchResult"; requestId: string; result?: NodeDefinitionSearch; error?: string }
    | { type: "graphAction"; action: "addNode" | "editNode" | "createPackage"; instanceId?: string };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "setExpandedModules"; modules: NodeAddress[] }
    | { type: "setTimelineSnapMode"; mode: TimelineGridMode }
    | { type: "playbackToggle" }
    | { type: "resetGraphState" }
    | { type: "playbackSeek"; position: string }
    | { type: "playbackDrag"; phase: PlaybackDragPhase; position: string; releaseId?: string }
    | ObservationHostRequest
    | { type: "observationResync" }
    | { type: "observationProfile"; summary: ObservationProfileSummary }
    | { type: "presentationError"; details: string }
    | { type: "clientInteractionTrace"; traceId: string; recording: InteractionRecording }
    | { type: "graphUiProfile"; profileId: string; summary: GraphUiProfileSummary }
    | { type: "playbackProfileClick"; profileId: string }
    | { type: "cursorReturnTrace"; traceId: string; step: string; details?: TraceDetails }
    | NodeEventHostRequest
    | { type: "graphInteractionHint"; hint: StatusHint | null }
    | CursorReturnMessage
    | { type: "manualControlAction"; action: ManualControlAction }
    | { type: "deleteSelectedLanes"; instanceIds: string[] }
    | { type: "duplicateSelectedLanes"; copies: GraphLaneCopy[]; requestId: string }
    | { type: "toggleManualInput"; control: ManualControlId }
    | { type: "projectHistory"; direction: "undo" | "redo" }
    | { type: "projectHistoryInterrupted"; requestId: number }
    | { type: "readLaneViews"; reads: Array<{ requestId: number; version: string }> }
    | { type: "copyNodeDebugDetails"; nodeKey: string }
    | { type: "openSymbolicNodeSource"; nodeKey: string }
    | { type: "flushProjectAutosave" }
    | { type: "knobClipboard"; requestId: number; text?: string }
    | { type: "renameGraphLane"; instanceId: string; displayName: string; previousDisplayName: string }
    | { type: "connectGraphPorts"; first: GraphPortEndpoint; second: GraphPortEndpoint; detach?: true }
    | { type: "editGraphConnectionEnd"; edit: GraphConnectionChange; requestId?: number }
    | { type: "searchNodeDefinitions"; requestId: string; source: string; cursor: number }
    | { type: "createGraphNode"; definitionId: string; displayName: string; configurationSource: string; afterInstanceId?: string }
    | { type: "updateGraphNode"; instanceId: string; displayName: string; configurationSource: string;
        previousDisplayName: string; previousConfigurationSource: string; requestId?: number }
    | { type: "createGraphPackage"; name: string }
    | ({ type: "saveGraphPresentation"; requestId: number } & GraphPresentationPatch);

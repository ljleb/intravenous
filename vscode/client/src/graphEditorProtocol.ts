import type { ObservationHostRequest, ObservationResponse, ObservationUpdate } from "./observations";
import type { ManualControlAction, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import { readLayout, type GraphLayout, type GraphScope, type GraphViewState, type NodeAddress, type PresentationState } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionEdit, GraphPortEndpoint } from "./graphConnectionEditing";
import type { NodeEventHostRequest, NodeEventResponse } from "./nodeEventMessaging";
import type { PlaybackDragPhase, PlaybackSnapshot } from "./playback";
import type { StatusHint } from "./statusBarText";

export type CursorWarp = {
    originX: number; originY: number;
    screenOrigin?: { x: number; y: number };
    deltaX: number; deltaY: number;
    expiresAt: number;
};

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
export type GraphEditorHostMessage = { type: "graphState"; state: GraphEditorState }
    | { type: "playbackUpdate"; snapshot: PlaybackSnapshot | null; requestedPosition: string | null }
    | ObservationResponse
    | { type: "observationVisibility"; visible: boolean }
    | { type: "observationUpdate"; update: ObservationUpdate }
    | NodeEventResponse
    | { type: "manualControlState"; interactions: ManualControlInteractionState[]; live: ManualControlLiveValue[] }
    | { type: "graphWindowFocus"; focused: boolean }
    | { type: "cursorReturnCompleted"; requestId: number }
    | { type: "graphAction"; action: "addNode" | "editNode" | "createPackage"; instanceId?: string };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "playbackToggle" }
    | { type: "playbackSeek"; position: string }
    | { type: "playbackDrag"; phase: PlaybackDragPhase; position: string }
    | ObservationHostRequest
    | NodeEventHostRequest
    | { type: "graphInteractionHint"; hint: StatusHint | null }
    | { type: "warpCursorAfterPointDrag" | "warpCursorAfterMiddleDrag"; warp: CursorWarp; requestId: number }
    | { type: "cursorReturnSkipped"; detail: string }
    | { type: "manualControlAction"; action: ManualControlAction }
    | { type: "deleteSelectedLanes"; instanceIds: string[] }
    | { type: "setManualInputEnabled"; target: NodeAddress; inputName: string; enabled: boolean }
    | { type: "projectHistory"; direction: "undo" | "redo" }
    | { type: "copyNodeDebugDetails"; nodeKey: string }
    | { type: "connectGraphPorts"; first: GraphPortEndpoint; second: GraphPortEndpoint }
    | { type: "editGraphConnectionEnd"; edit: GraphConnectionEdit }
    | { type: "createGraphNode"; definitionId: string; displayName: string; configurationSource: string }
    | { type: "updateGraphNode"; instanceId: string; displayName: string; configurationSource: string;
        previousDisplayName: string; previousConfigurationSource: string }
    | { type: "createGraphPackage"; name: string }
    | SaveGraphLayoutMessage;

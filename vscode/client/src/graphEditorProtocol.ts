import type { ObservationHostRequest, ObservationResponse, ObservationUpdate } from "./observations";
import type { ManualControlAction, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import type { ManualControlHint } from "./manualControlGesture";
import type { GraphLayout, GraphScope } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionEdit, GraphPortEndpoint } from "./graphConnectionEditing";
import type { NodeEventHostRequest, NodeEventResponse } from "./nodeEventMessaging";
import type { PlaybackDragPhase, PlaybackSnapshot } from "./playback";

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
    | { type: "graphAction"; action: "addNode" | "editNode" | "createPackage"; instanceId?: string };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "playbackToggle" }
    | { type: "playbackSeek"; position: string }
    | { type: "playbackDrag"; phase: PlaybackDragPhase; position: string }
    | ObservationHostRequest
    | NodeEventHostRequest
    | { type: "graphInteractionHint"; hint: ManualControlHint | null }
    | { type: "manualControlAction"; action: ManualControlAction }
    | { type: "projectHistory"; direction: "undo" | "redo" }
    | { type: "copyNodeDebugDetails"; nodeKey: string }
    | { type: "connectGraphPorts"; first: GraphPortEndpoint; second: GraphPortEndpoint }
    | { type: "editGraphConnectionEnd"; edit: GraphConnectionEdit }
    | { type: "createGraphNode"; definitionId: string; displayName: string; configurationSource: string }
    | { type: "updateGraphNode"; instanceId: string; displayName: string; configurationSource: string;
        previousDisplayName: string; previousConfigurationSource: string }
    | { type: "createGraphPackage"; name: string }
    | { type: "saveGraphLayout"; layout: GraphLayout };

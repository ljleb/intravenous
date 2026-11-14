import type { ManualControlAction, ManualControlInteractionState } from "./manualControls";
import type { ManualControlHint } from "./manualControlGesture";
import type { GraphLayout, GraphScope } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";
import type { GraphConnectionEdit, GraphPortEndpoint } from "./graphConnectionEditing";
import type { KnobPointerTraceReport } from "./knobPointerTrace";

export type GraphEditorState = {
    sessionStatus: LiveGraphSessionStatus;
    scope: GraphScope | null;
    layout: GraphLayout;
    definitions: string[];
    error: string;
};
export type GraphEditorAction = "addNode" | "editNode" | "deleteNode" | "openNodeSource" | "createPackage" | "deleteConnection" | "undo" | "redo" | "recordKnobPointer";
export type GraphEditorHostMessage = { type: "graphState"; state: GraphEditorState }
    | { type: "manualControlState"; interactions: ManualControlInteractionState[] }
    | { type: "graphWindowFocus"; focused: boolean }
    | { type: "knobPointerTraceToggle" }
    | { type: "graphAction"; action: "addNode" | "editNode" | "createPackage"; instanceId?: string };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "graphInteractionHint"; hint: ManualControlHint | null }
    | { type: "graphInteractionDiagnostic"; message: string }
    | { type: "knobPointerTrace"; report: KnobPointerTraceReport }
    | { type: "knobPointerTraceStatus"; state: "armed" | "recording" }
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

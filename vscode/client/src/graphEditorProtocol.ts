import type { GraphLayout, GraphScope } from "./graphScope";
import type { LiveGraphSessionStatus } from "./liveGraphProtocol";

export type GraphEditorState = {
    sessionStatus: LiveGraphSessionStatus;
    scope: GraphScope | null;
    layout: GraphLayout;
    error: string;
};
export type GraphEditorHostMessage = { type: "graphState"; state: GraphEditorState };
export type GraphEditorWebviewMessage = { type: "graphReady" }
    | { type: "saveGraphLayout"; layout: GraphLayout };

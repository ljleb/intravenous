import type { VirtualNode } from "./graphModel";

// Messages between the extension host and the Vue webview. This module is
// imported by both sides; it must contain only types and pure functions.

export type LiveGraphSessionStatus =
    | "connecting"
    | "attached"
    | "reconnecting";

export type LiveGraphInstance = {
    instanceId: string;
    definitionId: string;
    displayName: string;
    moduleId: string;
    packageRoot: string;
    realized: boolean;
};

// The complete state the host publishes to the webview. The host always
// sends the whole state, so a webview that (re)loads after a reconnect or
// while hidden needs no replay.
export type LiveGraphViewState = {
    sessionStatus: LiveGraphSessionStatus;
    instances: LiveGraphInstance[];
    selectedInstanceId: string | null;
    // Package root of the active module.cpp editor, which enables
    // "Duplicate".
    packageRoot: string | null;
    // Virtual nodes from the backend's source-introspection read model.
    nodes: VirtualNode[];
};

export type LiveGraphStateMessage = {
    type: "state";
    state: LiveGraphViewState;
};

export type LiveGraphHostMessage = LiveGraphStateMessage;

export type LiveGraphReadyMessage = {
    type: "ready";
};

export type LiveGraphSelectInstanceMessage = {
    type: "selectInstance";
    instanceId: string | null;
};

export type LiveGraphCreateInstanceMessage = {
    type: "createInstance";
};

export type LiveGraphWebviewMessage =
    | LiveGraphReadyMessage
    | LiveGraphSelectInstanceMessage
    | LiveGraphCreateInstanceMessage;

// The webview messages that WorkspaceSession acts on.
export type LiveGraphControlMessage =
    | LiveGraphSelectInstanceMessage
    | LiveGraphCreateInstanceMessage;

export type LiveGraphControlHandler = (message: LiveGraphControlMessage) => Promise<void>;

export function isLiveGraphWebviewMessage(message: unknown): message is LiveGraphWebviewMessage {
    if (!message || typeof message !== "object") {
        return false;
    }

    const candidate = message as Record<string, unknown>;
    if (candidate.type === "selectInstance") {
        return candidate.instanceId == null || typeof candidate.instanceId === "string";
    }
    return candidate.type === "ready" || candidate.type === "createInstance";
}

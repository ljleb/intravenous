import { shallowRef } from "vue";

import type {
    LiveGraphHostMessage,
    LiveGraphViewState,
    LiveGraphWebviewMessage,
} from "../src/liveGraphProtocol";

// Webview-local UI state that survives the webview being hidden or
// reloaded. It never reaches the backend.
export type WebviewLocalState = {
    viewChoice?: string;
};

const vscode = acquireVsCodeApi<WebviewLocalState>();

// The latest complete state from the extension host. It is replaced, never
// mutated, so a shallow ref is enough.
export const hostState = shallowRef<LiveGraphViewState>({
    sessionStatus: "connecting",
    instances: [],
    selectedInstanceId: null,
    packageRoot: null,
    nodes: [],
});

export function postToHost(message: LiveGraphWebviewMessage): void {
    vscode.postMessage(message);
}

export function loadLocalState(): WebviewLocalState {
    return vscode.getState() ?? {};
}

export function saveLocalState(state: WebviewLocalState): void {
    vscode.setState(state);
}

// Listens for host state, then asks the host for its current state.
export function connectToHost(): void {
    window.addEventListener("message", (event: MessageEvent<unknown>) => {
        const message = event.data as Partial<LiveGraphHostMessage> | null;
        if (!message || typeof message !== "object" || message.type !== "state" || !message.state) {
            return;
        }
        hostState.value = message.state;
    });
    postToHost({ type: "ready" });
}

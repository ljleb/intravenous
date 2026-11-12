/// <reference types="vite/client" />

// Provided by the VS Code webview runtime; callable once per page.
declare function acquireVsCodeApi<State = unknown>(): {
    postMessage(message: unknown): void;
    getState(): State | undefined;
    setState(state: State): void;
};

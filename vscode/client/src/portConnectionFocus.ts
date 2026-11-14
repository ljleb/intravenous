import type { GraphEditorHostMessage } from "./graphEditorProtocol";

// A webview blur can occur during native window activation while its pointer
// remains captured. Only the host knows whether the native window lost focus.
export function listenPortConnectionFocus(target: EventTarget,
    documentBlurred: () => void, nativeWindowFocusChanged: (focused: boolean) => void): () => void {
    const blurred = () => documentBlurred();
    const message = (event: Event) => {
        const data = (event as MessageEvent<GraphEditorHostMessage>).data;
        if (data?.type === "graphWindowFocus") nativeWindowFocusChanged(data.focused);
    };
    target.addEventListener("blur", blurred);
    target.addEventListener("message", message);
    return () => {
        target.removeEventListener("blur", blurred);
        target.removeEventListener("message", message);
    };
}

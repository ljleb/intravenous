import { createApp, ref } from "vue";

import App from "./App.vue";
import GraphEditor from "./graph/GraphEditor.vue";
import { connectToHost } from "./host";
import "./style.css";
import { NodeEventMessaging, type NodeEventResponse } from "../src/nodeEventMessaging";
import { nodeEventMessagingKey } from "./presentation/nodeEventContext";
import { webviewApi } from "./host";
import { AutomationCurveEditing, type CurveAxes } from "../src/automationCurveEditing";
import { automationEditingKey } from "./presentation/automationEditingContext";
import type { NodeAddress } from "../src/graphScope";
import { projectHistoryShortcut } from "../src/projectHistoryShortcut";

const nodeEvents = new NodeEventMessaging((request) => webviewApi.postMessage(request), () => crypto.randomUUID());
const automationVersion = ref(0);
let curveCapture: { element: Element; pointerId: number } | null = null;
function releaseCurveCapture() {
    const capture = curveCapture; curveCapture = null;
    if (capture?.element.hasPointerCapture(capture.pointerId)) capture.element.releasePointerCapture(capture.pointerId);
}
const automation = new AutomationCurveEditing(nodeEvents, () => {
    ++automationVersion.value;
    if (automation.pointerId() === null) releaseCurveCapture();
});
const automationContext = { edits: automation, version: automationVersion,
    begin(event: PointerEvent, target: NodeAddress, pointId: number, axes: CurveAxes) {
        if (event.button !== 0 || !automation.begin(target, pointId, event.pointerId, axes, { x: event.clientX, y: event.clientY })) return;
        event.preventDefault(); event.stopPropagation();
        const element = event.target instanceof Element ? event.target.closest(".graph-editor") : null;
        if (element) { element.setPointerCapture(event.pointerId); curveCapture = { element, pointerId: event.pointerId }; }
    } };
window.addEventListener("message", (event) => {
    const message = event.data;
    if (message?.type === "nodeEventResult") nodeEvents.receive(message as NodeEventResponse);
    else if (message?.type === "graphState" || message?.type === "state") {
        if (message.state?.sessionStatus !== "running") { nodeEvents.disconnected(); automation.disconnected(); }
        else if (message.type === "graphState") automation.reconcile(message.state.scope);
    } else if (message?.type === "graphWindowFocus" && !message.focused) void automation.finish();
});
window.addEventListener("pagehide", () => { nodeEvents.disconnected(); automation.disconnected(); });

if (document.body.dataset.surface === "graphEditor") {
    createApp(GraphEditor).provide(nodeEventMessagingKey, nodeEvents).provide(automationEditingKey, automationContext).mount("#app");
} else {
    createApp(App).provide(nodeEventMessagingKey, nodeEvents).provide(automationEditingKey, automationContext).mount("#app");
    connectToHost();
}

// The graph installs its pointer-lock exit guard during setup. Register these
// handlers afterward so displaced events never reach point gestures first.
window.addEventListener("pointermove", (event) => {
    if (automation.pointerId() !== event.pointerId) return;
    event.preventDefault(); event.stopImmediatePropagation();
    if (!(event.buttons & 1)) { void automation.finish(); return; }
    automation.move(event.pointerId, { x: event.clientX, y: event.clientY });
}, true);
const finishAutomation = (event: PointerEvent) => {
    if (automation.pointerId() !== event.pointerId) return;
    if (event.type === "pointerup") automation.move(event.pointerId, { x: event.clientX, y: event.clientY });
    event.stopImmediatePropagation();
    void automation.finish();
};
window.addEventListener("pointerup", finishAutomation, true);
window.addEventListener("pointercancel", finishAutomation, true);
window.addEventListener("keydown", (event) => {
    const editingText = event.target instanceof Element
        && !!event.target.closest("input, textarea, select, [contenteditable]:not([contenteditable=false]), [role=textbox]");
    if (projectHistoryShortcut(event, editingText)) automation.abandon();
    else if (event.key === "Escape") void automation.finish();
}, true);

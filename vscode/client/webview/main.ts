import { createApp, ref } from "vue";

import App from "./App.vue";
import GraphEditor from "./graph/GraphEditor.vue";
import { connectToHost } from "./host";
import "./style.css";
import { NodeEventMessaging, type NodeEventResponse } from "../src/nodeEventMessaging";
import { nodeEventMessagingKey } from "./presentation/nodeEventContext";
import { webviewApi } from "./host";
import { AutomationCurveEditing } from "../src/automationCurveEditing";
import { automationEditingKey } from "./presentation/automationEditingContext";

const nodeEvents = new NodeEventMessaging((request) => webviewApi.postMessage(request), () => crypto.randomUUID());
const automationVersion = ref(0);
const automation = new AutomationCurveEditing(nodeEvents, () => { ++automationVersion.value; });
const automationContext = { edits: automation, version: automationVersion };
window.addEventListener("message", (event) => {
    const message = event.data;
    if (message?.type === "nodeEventResult") nodeEvents.receive(message as NodeEventResponse);
    else if (message?.type === "graphState" || message?.type === "state") {
        if (message.state?.sessionStatus !== "running") { nodeEvents.disconnected(); automation.disconnected(); }
        else if (message.type === "graphState") automation.reconcile(message.state.scope);
    }
});
window.addEventListener("pagehide", () => { nodeEvents.disconnected(); automation.disconnected(); });

if (document.body.dataset.surface === "graphEditor") {
    createApp(GraphEditor).provide(nodeEventMessagingKey, nodeEvents).provide(automationEditingKey, automationContext).mount("#app");
} else {
    createApp(App).provide(nodeEventMessagingKey, nodeEvents).provide(automationEditingKey, automationContext).mount("#app");
    connectToHost();
}

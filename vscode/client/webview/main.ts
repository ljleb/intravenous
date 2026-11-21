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
import { BeatGridEditing } from "../src/beatGridEditing";
import { beatGridEditingKey } from "./presentation/beatGridEditingContext";
import { createPresentationFrames, presentationFramesKey } from "./presentation/presentationFrames";

const nodeEvents = new NodeEventMessaging((request) => webviewApi.postMessage(request), () => crypto.randomUUID());
const automationVersion = ref(0);
const automation = new AutomationCurveEditing(nodeEvents, () => { ++automationVersion.value; });
const automationContext = { edits: automation, version: automationVersion };
const beatsVersion = ref(0);
const beats = new BeatGridEditing(nodeEvents, () => { ++beatsVersion.value; });
const beatsContext = { edits: beats, version: beatsVersion };
function disconnectEditing() {
    nodeEvents.disconnected();
    automation.disconnected();
    beats.disconnected();
}
window.addEventListener("message", (event) => {
    const message = event.data;
    if (message?.type === "nodeEventResult") nodeEvents.receive(message as NodeEventResponse);
    else if (message?.type === "graphState" || message?.type === "state") {
        if (message.state?.sessionStatus !== "running") disconnectEditing();
        else if (message.type === "graphState") {
            automation.reconcile(message.state.scope);
            beats.reconcile(message.state.scope);
        }
    }
});
window.addEventListener("pagehide", disconnectEditing);

const graphEditor = document.body.dataset.surface === "graphEditor";
createApp(graphEditor ? GraphEditor : App).provide(nodeEventMessagingKey, nodeEvents)
    .provide(presentationFramesKey, createPresentationFrames())
    .provide(automationEditingKey, automationContext).provide(beatGridEditingKey, beatsContext).mount("#app");
if (!graphEditor) connectToHost();

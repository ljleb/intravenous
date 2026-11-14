import { createApp } from "vue";

import App from "./App.vue";
import GraphEditor from "./graph/GraphEditor.vue";
import { connectToHost } from "./host";
import "./style.css";

if (document.body.dataset.surface === "graphEditor") {
    createApp(GraphEditor).mount("#app");
} else {
    createApp(App).mount("#app");
    connectToHost();
}

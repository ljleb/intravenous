import { createApp } from "vue";

import App from "./App.vue";
import { connectToHost } from "./host";
import "./style.css";

createApp(App).mount("#app");
connectToHost();

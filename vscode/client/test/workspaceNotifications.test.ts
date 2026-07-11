import test from "node:test";
import assert from "node:assert/strict";

import { WorkspaceNotificationRouter } from "../src/workspaceNotifications";

test("WorkspaceNotificationRouter dispatches to subscribed handlers in order", async () => {
    const router = new WorkspaceNotificationRouter();
    const seen: string[] = [];

    router.subscribe("server.status", async (params) => {
        seen.push(`first:${String(params.code)}`);
    });
    router.subscribe("server.status", async (params) => {
        seen.push(`second:${String(params.code)}`);
    });

    await router.dispatch("server.status", { code: "rebuildFinished" });

    assert.deepEqual(seen, [
        "first:rebuildFinished",
        "second:rebuildFinished",
    ]);
});

test("WorkspaceNotificationRouter unsubscribes cleanly", async () => {
    const router = new WorkspaceNotificationRouter();
    let count = 0;

    const subscription = router.subscribe("timeline.laneViewUpdated", async () => {
        count += 1;
    });

    await router.dispatch("timeline.laneViewUpdated", { viewId: "view-1" });
    subscription.dispose();
    await router.dispatch("timeline.laneViewUpdated", { viewId: "view-1" });

    assert.equal(count, 1);
});

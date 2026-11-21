import test from "node:test";
import assert from "node:assert/strict";

import { VirtualNode } from "../src/graphModel";
import { liveGraphInstances, upsertVirtualNodes } from "../src/liveGraphModel";
import { instanceLabel, visibleNodes } from "../webview/liveGraphView";

test("liveGraphInstances keeps only records with an instance ID", () => {
    const instances = liveGraphInstances([
        {
            instanceId: "instance-1",
            definitionId: "definition-1",
            moduleId: "iv.project.simple_sine",
            packageRoot: "/tmp/simple_sine",
            realized: true,
        },
        { definitionId: "definition-2" },
        { instanceId: "" },
    ]);

    assert.deepEqual(instances, [{
        instanceId: "instance-1",
        definitionId: "definition-1",
        displayName: "",
        moduleId: "iv.project.simple_sine",
        packageRoot: "/tmp/simple_sine",
        realized: true,
    }]);
});

test("instanceLabel prefers the display name and marks unrealized instances", () => {
    const [instance] = liveGraphInstances([{
        instanceId: "instance-1",
        moduleId: "iv.project.simple_sine",
        realized: true,
    }]);
    assert.equal(instanceLabel(instance), "iv.project.simple_sine • instance-1");
    assert.equal(
        instanceLabel({ ...instance, displayName: "Lead", realized: false }),
        "Lead [loading]");
});

test("upsertVirtualNodes replaces whole instances and then nodes by ID", () => {
    const current: VirtualNode[] = [
        { id: "a:1", instanceId: "a", kind: "Old" },
        { id: "a:2", instanceId: "a", kind: "Removed" },
        { id: "b:1", instanceId: "b", kind: "Kept" },
        { id: "c:1", instanceId: "c", kind: "Old" },
    ];
    const updated = upsertVirtualNodes(
        current,
        [
            { id: "a:1", instanceId: "a", kind: "New" },
            { id: "c:1", instanceId: "c", kind: "New" },
        ],
        ["a"]);

    assert.deepEqual(
        updated.map((node) => `${node.id}=${node.kind}`).sort(),
        ["a:1=New", "b:1=Kept", "c:1=New"]);
});

test("visibleNodes falls back to every node when the selection has none", () => {
    const nodes: VirtualNode[] = [
        { id: "a:1", instanceId: "a" },
        { id: "b:1", instanceId: "b" },
    ];
    assert.deepEqual(visibleNodes(nodes, "b").map((node) => node.id), ["b:1"]);
    assert.deepEqual(visibleNodes(nodes, "missing").map((node) => node.id), ["a:1", "b:1"]);
    assert.deepEqual(visibleNodes(nodes, null).map((node) => node.id), ["a:1", "b:1"]);
});

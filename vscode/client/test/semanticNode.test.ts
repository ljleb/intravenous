import test from "node:test";
import assert from "node:assert/strict";

import { VirtualNode } from "../src/graphModel";
import {
    bindingSummary,
    nodeDescription,
    nodeIdentitySummary,
    portDescription,
    portGroups,
    portTypeSummary,
} from "../webview/presentation/nodeSummary";
import { semanticNodeFromVirtualNode } from "../webview/presentation/semanticNode";

function oscillator(): VirtualNode {
    const sample = { type: "Sample", rank: 0, channelCapability: "audio" as const };
    return {
        id: "instance-1::node-1",
        instanceId: "instance-1",
        kind: "Oscillator",
        definitionId: "iv.test.oscillator",
        sourceIdentity: "module@Oscillator",
        memberCount: 2,
        continuousInputs: [{
            index: 1,
            name: "frequency",
            connectivity: "disconnected",
            binding: "interface",
            valueTypes: [{ ...sample, defaultValue: 0.5, minValue: 0, maxValue: 1 }],
            channelType: "mono",
        }],
        continuousOutputs: [{
            index: 2,
            name: "out",
            connectivity: "connected",
            binding: "interface",
            valueTypes: [sample],
            channelType: "mono",
        }],
        eventInputs: [{
            index: 3,
            name: "gate",
            connectivity: "mixed",
            binding: "interface",
            eventType: "trigger",
        }],
        eventOutputs: [{
            index: 4,
            name: "trig",
            connectivity: "connected",
            binding: "interface",
            eventType: "trigger",
        }],
        members: [{
            index: 7,
            backingNodeId: "backing-1",
            definitionId: "iv.test.oscillator",
            continuousInputs: [{
                index: 1,
                name: "frequency",
                connectivity: "disconnected",
                binding: "virtualInput",
                valueTypes: [sample],
            }],
            eventInputs: [{
                index: 3,
                name: "gate",
                connectivity: "mixed",
                binding: "mixed",
                eventType: "trigger",
            }],
        }],
    };
}

test("semanticNodeFromVirtualNode orders inputs before outputs and keeps port indices", () => {
    const node = semanticNodeFromVirtualNode(oscillator());

    assert.equal(node.definitionId, "iv.test.oscillator");
    assert.deepEqual(
        node.ports.map((port) => `${port.direction}:${port.kind}:${port.index}:${port.name}`),
        [
            "input:continuous:1:frequency",
            "input:event:3:gate",
            "output:continuous:2:out",
            "output:event:4:trig",
        ]);
    assert.equal(node.members.length, 1);
    assert.equal(node.members[0].index, 7);
    // A member without its own kind takes the virtual node's kind.
    assert.equal(node.members[0].kind, "Oscillator");
    assert.deepEqual(
        node.members[0].ports.map((port) => `${port.direction}:${port.binding}`),
        ["input:virtualInput", "input:mixed"]);
});

test("port summaries describe types, bindings, and connectivity", () => {
    const node = semanticNodeFromVirtualNode(oscillator());
    const [frequency, gate] = node.ports;

    assert.equal(portTypeSummary(frequency), "Sample • mono");
    assert.equal(portTypeSummary(gate), "trigger");
    assert.equal(portDescription(frequency), "Sample • mono • graph interface • disconnected");
    assert.equal(bindingSummary("virtualInput", "disconnected"), "follow virtual input");
    assert.equal(bindingSummary("virtualOutput", "connected"), "virtual output");
    assert.equal(bindingSummary("mixed", "mixed"), "mixed built-in and virtual input");
    assert.equal(bindingSummary("none", "connected"), "built-in connection");
    assert.equal(bindingSummary("none", "disconnected"), "disconnected");

    assert.deepEqual(
        portGroups(node.ports).map((group) => `${group.label}:${group.ports.length}`),
        [
            "continuous inputs:1",
            "event inputs:1",
            "continuous outputs:1",
            "event outputs:1",
        ]);
});

test("node identity comes from the source identity, then the node ID", () => {
    const node = semanticNodeFromVirtualNode(oscillator());
    assert.equal(nodeIdentitySummary(node), "Oscillator");
    assert.equal(nodeDescription(node), "virtual • Oscillator • 2 concrete nodes");
    assert.equal(
        nodeIdentitySummary({ ...node, sourceIdentity: "", id: "instance-1::voice@Filter" }),
        "Filter");
});

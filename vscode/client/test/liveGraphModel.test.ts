import test from "node:test";
import assert from "node:assert/strict";

import {
    serializeLiveGraphInstances,
    serializeLiveGraphNodes,
} from "../src/liveGraphModel";
import { VirtualNode } from "../src/graphModel";

function continuousNode(): VirtualNode {
    return {
        id: "node-1",
        kind: "Oscillator",
        packageIdentity: "module@Oscillator",
        memberCount: 2,
        continuousInputs: [{
            index: 1,
            name: "frequency",
            connectivity: "disconnected",
            binding: "interface",
            valueTypes: [{
                type: "Sample",
                rank: 0,
                channelCapability: "audio",
                defaultValue: 0.5,
                minValue: 0,
                maxValue: 1,
            }],
            channelType: "mono",
        }],
        continuousOutputs: [{
            index: 2,
            name: "out",
            connectivity: "connected",
            binding: "interface",
            valueTypes: [{
                type: "Sample",
                rank: 0,
                channelCapability: "audio",
            }],
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
            kind: "Oscillator",
            continuousInputs: [{
                index: 1,
                name: "frequency",
                connectivity: "disconnected",
                binding: "virtualInput",
                valueTypes: [{
                    type: "Sample",
                    rank: 0,
                    channelCapability: "audio",
                    defaultValue: 0.5,
                    minValue: 0,
                    maxValue: 1,
                }],
                channelType: "mono",
            }],
            continuousOutputs: [{
                index: 2,
                name: "out",
                connectivity: "connected",
                binding: "virtualOutput",
                valueTypes: [{
                    type: "Sample",
                    rank: 0,
                    channelCapability: "audio",
                }],
                channelType: "mono",
            }],
            eventInputs: [{
                index: 3,
                name: "gate",
                connectivity: "mixed",
                binding: "mixed",
                eventType: "trigger",
            }],
            eventOutputs: [{
                index: 4,
                name: "trig",
                connectivity: "connected",
                binding: "virtualOutput",
                eventType: "trigger",
            }],
        }],
    };
}

test("serializeLiveGraphNodes exposes declared types and static bindings", () => {
    const serialized = serializeLiveGraphNodes([continuousNode()]);
    assert.equal(serialized.length, 1);

    const node = serialized[0];
    const virtualContinuousInput = node.groups[0].ports[0];
    const virtualContinuousOutput = node.groups[1].ports[0];
    const virtualEventInput = node.groups[2].ports[0];
    const virtualEventOutput = node.groups[3].ports[0];
    const memberContinuousInput = node.members[0].groups[0].ports[0];

    assert.equal(virtualContinuousInput.index, 1);
    assert.equal(virtualContinuousOutput.index, 2);
    assert.equal(virtualEventInput.index, 3);
    assert.equal(virtualEventOutput.index, 4);
    assert.equal(node.members[0].index, 7);
    assert.equal(memberContinuousInput.index, 1);

    assert.equal(virtualContinuousInput.typeSummary, "Sample • mono");
    assert.equal(virtualContinuousInput.bindingSummary, "graph interface");
    assert.equal(virtualContinuousOutput.typeSummary, "Sample • mono");
    assert.equal(virtualEventInput.typeSummary, "trigger");
    assert.equal(virtualEventInput.bindingSummary, "graph interface");
    assert.equal(virtualEventOutput.typeSummary, "trigger");
    assert.equal(memberContinuousInput.bindingSummary, "follow virtual input");
    assert.equal(
        node.members[0].groups[2].ports[0].bindingSummary,
        "mixed built-in and virtual input");
});

test("serializeLiveGraphNodes reports concrete and virtual bindings", () => {
    const node: VirtualNode = {
        id: "node-1",
        kind: "Module",
        continuousOutputs: [{
            index: 2,
            name: "mix",
            connectivity: "connected",
            binding: "interface",
            valueTypes: [{
                type: "Sample",
                rank: 0,
                channelCapability: "audio",
            }],
        }],
        members: [{
            index: 1,
            backingNodeId: "backing-1",
            kind: "Member",
            continuousInputs: [{
                index: 3,
                name: "in",
                connectivity: "connected",
                binding: "none",
                valueTypes: [{
                    type: "Sample",
                    rank: 0,
                    channelCapability: "audio",
                }],
            }],
            continuousOutputs: [{
                index: 2,
                name: "mix",
                connectivity: "connected",
                binding: "virtualOutput",
                valueTypes: [{
                    type: "Sample",
                    rank: 0,
                    channelCapability: "audio",
                }],
            }],
        }],
    };

    const serialized = serializeLiveGraphNodes([node]);
    const memberContinuousInput = serialized[0].members[0].groups[0].ports[0];
    const memberContinuousOutput = serialized[0].members[0].groups[1].ports[0];

    assert.equal(memberContinuousInput.bindingSummary, "built-in connection");
    assert.equal(memberContinuousOutput.bindingSummary, "virtual output");
});

test("serializeLiveGraphInstances builds stable dropdown labels", () => {
    const serialized = serializeLiveGraphInstances([{
        instanceId: "instance-1",
        definitionId: "definition-1",
        moduleId: "iv.project.simple_sine",
        packageRoot: "/tmp/simple_sine",
        realized: true,
    }]);

    assert.deepEqual(serialized, [{
        instanceId: "instance-1",
        definitionId: "definition-1",
        moduleId: "iv.project.simple_sine",
        packageRoot: "/tmp/simple_sine",
        realized: true,
        label: "iv.project.simple_sine • instance-1",
    }]);
});

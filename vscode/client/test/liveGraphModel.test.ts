import test from "node:test";
import assert from "node:assert/strict";

import {
    serializeLiveGraphInstances,
    serializeLiveGraphNodes,
} from "../src/liveGraphModel";
import { VirtualNode } from "../src/graphModel";

function sampleNode(): VirtualNode {
    return {
        id: "node-1",
        kind: "Oscillator",
        sourceIdentity: "module@Oscillator",
        memberCount: 2,
        sampleInputs: [{
            ordinal: 1,
            name: "frequency",
            connectivity: "disconnected",
            defaultValue: 0.5,
            currentValue: 0.25,
            stateValue: "overridden",
        }],
        sampleOutputs: [{
            ordinal: 2,
            name: "out",
            connectivity: "connected",
            stateValue: "disconnected",
        }],
        eventInputs: [{
            ordinal: 3,
            name: "gate",
            connectivity: "mixed",
            stateValue: "default",
        }],
        eventOutputs: [{
            ordinal: 4,
            name: "trig",
            connectivity: "connected",
            stateValue: "disconnected",
        }],
        members: [{
            ordinal: 7,
            backingNodeId: "backing-1",
            kind: "Oscillator",
            sampleInputs: [{
                ordinal: 1,
                name: "frequency",
                connectivity: "disconnected",
                defaultValue: 0.5,
                currentValue: 0.25,
                hasConcreteOverride: false,
                stateValue: "virtualFollow",
            }],
            sampleOutputs: [{
                ordinal: 2,
                name: "out",
                connectivity: "connected",
                stateValue: "disconnected",
            }],
            eventInputs: [{
                ordinal: 3,
                name: "gate",
                connectivity: "mixed",
                stateValue: "virtualFollow",
            }],
            eventOutputs: [{
                ordinal: 4,
                name: "trig",
                connectivity: "connected",
                stateValue: "disconnected",
            }],
        }],
    };
}

test("serializeLiveGraphNodes exposes all supported port state families", () => {
    const serialized = serializeLiveGraphNodes([sampleNode()]);
    assert.equal(serialized.length, 1);

    const node = serialized[0];
    const virtualSampleInput = node.groups[0].ports[0];
    const virtualSampleOutput = node.groups[1].ports[0];
    const virtualEventInput = node.groups[2].ports[0];
    const virtualEventOutput = node.groups[3].ports[0];
    const memberSampleInput = node.members[0].groups[0].ports[0];

    assert.equal(virtualSampleInput.stateFamily, "sampleInput");
    assert.equal(virtualSampleInput.stateSummary, "knob value");
    assert.deepEqual(virtualSampleInput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(virtualEventInput.stateFamily, "eventInput");
    assert.equal(virtualEventInput.stateSummary, "default");
    assert.deepEqual(virtualEventInput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(virtualSampleOutput.stateFamily, "sampleOutput");
    assert.equal(virtualSampleOutput.stateSummary, "disconnected");
    assert.deepEqual(virtualSampleOutput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(virtualEventOutput.stateFamily, "eventOutput");
    assert.equal(virtualEventOutput.stateSummary, "disconnected");
    assert.deepEqual(virtualEventOutput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(memberSampleInput.stateSummary, "follow virtual value");
    assert.equal(memberSampleInput.resetState, null);
    assert.deepEqual(memberSampleInput.stateActions.map((action) => action.state), [
        "overridden",
        "timelineLane",
        "disconnected",
    ]);
});

test("serializeLiveGraphNodes treats default-connected concrete ports as connected", () => {
    const node: VirtualNode = {
        id: "node-1",
        kind: "Module",
        sampleOutputs: [{
            ordinal: 2,
            name: "mix",
            connectivity: "connected",
            stateValue: "timelineLane",
        }],
        members: [{
            ordinal: 1,
            backingNodeId: "backing-1",
            kind: "Member",
            sampleInputs: [{
                ordinal: 3,
                name: "in",
                connectivity: "connected",
                stateValue: "disconnected",
            }],
            sampleOutputs: [{
                ordinal: 2,
                name: "mix",
                connectivity: "connected",
                stateValue: "virtual",
            }],
        }],
    };

    const serialized = serializeLiveGraphNodes([node]);
    const memberSampleInput = serialized[0].members[0].groups[0].ports[0];
    const memberSampleOutput = serialized[0].members[0].groups[1].ports[0];

    assert.equal(memberSampleInput.stateSummary, "built-in connection");
    assert.equal(memberSampleInput.resetState, null);
    assert.deepEqual(memberSampleInput.stateActions.map((action) => action.state), [
        "overridden",
        "virtualFollow",
        "timelineLane",
    ]);

    assert.equal(memberSampleOutput.stateSummary, "virtual output");
    assert.equal(memberSampleOutput.resetState, null);
    assert.deepEqual(memberSampleOutput.stateActions.map((action) => action.state), [
        "timelineLane",
        "disconnected",
    ]);
});

test("serializeLiveGraphInstances builds stable dropdown labels", () => {
    const serialized = serializeLiveGraphInstances([{
        instanceId: "instance-1",
        definitionId: "definition-1",
        moduleId: "iv.project.simple_sine",
        moduleRoot: "/tmp/simple_sine",
        realized: true,
    }]);

    assert.deepEqual(serialized, [{
        instanceId: "instance-1",
        definitionId: "definition-1",
        moduleId: "iv.project.simple_sine",
        moduleRoot: "/tmp/simple_sine",
        realized: true,
        label: "iv.project.simple_sine • instance-1",
    }]);
});

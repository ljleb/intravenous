import test from "node:test";
import assert from "node:assert/strict";

import {
    serializeLiveGraphInstances,
    serializeLiveGraphNodes,
} from "../src/liveGraphModel";
import { LogicalNode } from "../src/graphModel";

function sampleNode(): LogicalNode {
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
                stateValue: "logicalFollow",
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
                stateValue: "logicalFollow",
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
    const logicalSampleInput = node.groups[0].ports[0];
    const logicalSampleOutput = node.groups[1].ports[0];
    const logicalEventInput = node.groups[2].ports[0];
    const logicalEventOutput = node.groups[3].ports[0];
    const memberSampleInput = node.members[0].groups[0].ports[0];

    assert.equal(logicalSampleInput.stateFamily, "sampleInput");
    assert.equal(logicalSampleInput.stateSummary, "knob value");
    assert.deepEqual(logicalSampleInput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(logicalEventInput.stateFamily, "eventInput");
    assert.equal(logicalEventInput.stateSummary, "default");
    assert.deepEqual(logicalEventInput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(logicalSampleOutput.stateFamily, "sampleOutput");
    assert.equal(logicalSampleOutput.stateSummary, "disconnected");
    assert.deepEqual(logicalSampleOutput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(logicalEventOutput.stateFamily, "eventOutput");
    assert.equal(logicalEventOutput.stateSummary, "disconnected");
    assert.deepEqual(logicalEventOutput.stateActions.map((action) => action.state), ["timelineLane"]);

    assert.equal(memberSampleInput.stateSummary, "follow logical value");
    assert.equal(memberSampleInput.resetState, null);
    assert.deepEqual(memberSampleInput.stateActions.map((action) => action.state), [
        "overridden",
        "timelineLane",
        "disconnected",
    ]);
});

test("serializeLiveGraphNodes treats default-connected concrete ports as connected", () => {
    const node: LogicalNode = {
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
                stateValue: "logical",
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
        "logicalFollow",
        "timelineLane",
    ]);

    assert.equal(memberSampleOutput.stateSummary, "logical output");
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

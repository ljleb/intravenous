import test from "node:test";
import assert from "node:assert/strict";

import {
    applySampleInputValueUpdate,
    clearSampleInputValueOverride,
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
        }],
        sampleOutputs: [{
            ordinal: 2,
            name: "out",
            connectivity: "connected",
        }],
        eventInputs: [{
            ordinal: 3,
            name: "gate",
            connectivity: "mixed",
        }],
        eventOutputs: [{
            ordinal: 4,
            name: "trig",
            connectivity: "connected",
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
            }],
            sampleOutputs: [{
                ordinal: 2,
                name: "out",
                connectivity: "connected",
            }],
            eventInputs: [{
                ordinal: 3,
                name: "gate",
                connectivity: "mixed",
            }],
            eventOutputs: [{
                ordinal: 4,
                name: "trig",
                connectivity: "connected",
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
    assert.deepEqual(logicalSampleInput.stateActions.map((action) => action.state), [
        "default",
        "disconnected",
        "timelineLane",
    ]);

    assert.equal(logicalEventInput.stateFamily, "eventInput");
    assert.deepEqual(logicalEventInput.stateActions.map((action) => action.state), [
        "default",
        "disconnected",
        "timelineLane",
    ]);

    assert.equal(logicalSampleOutput.stateFamily, "sampleOutput");
    assert.deepEqual(logicalSampleOutput.stateActions.map((action) => action.state), [
        "disconnected",
        "timelineLane",
    ]);

    assert.equal(logicalEventOutput.stateFamily, "eventOutput");
    assert.deepEqual(logicalEventOutput.stateActions.map((action) => action.state), [
        "disconnected",
        "timelineLane",
    ]);

    assert.equal(memberSampleInput.resetState, "default");
    assert.deepEqual(memberSampleInput.stateActions.map((action) => action.state), [
        "default",
        "logicalFollow",
        "disconnected",
        "timelineLane",
    ]);
});

test("applySampleInputValueUpdate updates logical and inherited member values", () => {
    const nodes = [sampleNode()];
    applySampleInputValueUpdate(nodes, "node-1", 1, 0.75, null);

    assert.equal(nodes[0].sampleInputs?.[0].currentValue, 0.75);
    assert.equal(nodes[0].members?.[0].sampleInputs?.[0].currentValue, 0.75);
});

test("clearSampleInputValueOverride restores inherited logical value", () => {
    const nodes = [sampleNode()];
    applySampleInputValueUpdate(nodes, "node-1", 1, 0.1, null);
    applySampleInputValueUpdate(nodes, "node-1", 1, 0.9, 7);

    clearSampleInputValueOverride(nodes, "node-1", 7, 1);

    assert.equal(nodes[0].members?.[0].sampleInputs?.[0].hasConcreteOverride, false);
    assert.equal(nodes[0].members?.[0].sampleInputs?.[0].currentValue, 0.1);
});

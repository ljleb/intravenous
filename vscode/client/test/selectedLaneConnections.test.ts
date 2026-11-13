import test from "node:test";
import assert from "node:assert/strict";
import { addressKey, type GraphScope, type GraphScopeNode, type GraphScopeConnection } from "../src/graphScope";
import { connectionEndKey, portEndpointKey, portSlotKey, selectedLaneConnections } from "../src/selectedLaneConnections";
import type { GraphPortEndpoint } from "../src/graphConnectionEditing";

function node(id: string): GraphScopeNode {
    return { address: { instanceId: id }, instance: { instanceId: id, definitionId: "test.node", realized: true },
        loadState: "ready", hasChildren: false,
        sampleInputs: [0, 1, 2].map((index) => ({ name: `in${index}`, index, channelType: "mono" })),
        sampleOutputs: [0, 1, 2].map((index) => ({ name: `out${index}`, index, channelType: "mono" })),
        eventInputs: [{ name: "gate", index: 0, eventType: "trigger" }],
        eventOutputs: [{ name: "events", index: 0, eventType: "trigger" }],
    };
}
function connection(id: string, source: string, output: number, target: string, input: number): GraphScopeConnection {
    return { id, applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: source, port: { index: output } }],
        inputs: [{ instanceId: target, port: { index: input } }],
    } };
}
function scope(connections: GraphScopeConnection[]): GraphScope {
    return { generation: 1, editable: true, nodes: [node("a"), node("b"), node("c")], connections };
}
const selected = addressKey({ instanceId: "a" });
function anchors(expanded: string[]) {
    return (port: GraphPortEndpoint) => {
        const group = portSlotKey(port.address, port.direction);
        const direct = expanded.includes(group);
        return { id: direct ? `port:${portEndpointKey(port)}` : `slot:${group}`,
            x: port.direction === "output" ? 30 : 10, y: direct ? port.index * 20 : 0 };
    };
}
const group = (id: string, direction: "input" | "output") => portSlotKey({ instanceId: id }, direction);

test("selected lane shows incoming and outgoing connections and excludes unrelated edges", () => {
    const graph = scope([
        connection("outgoing", "a", 0, "b", 0), connection("incoming", "c", 0, "a", 0),
        connection("unrelated", "b", 0, "c", 0),
    ]);
    const wires = selectedLaneConnections(graph, selected, anchors([]));
    assert.equal(wires.length, 2);
    assert.deepEqual(wires.flatMap((wire) => wire.connectionIds).sort(), ["incoming", "outgoing"]);
    assert.deepEqual(selectedLaneConnections(graph, null, anchors([])), []);
    assert.deepEqual(selectedLaneConnections(graph, addressKey({ instanceId: "removed" }), anchors([])), []);
});

test("hover expansion splits a wire to a grouped slot into the connected port rows", () => {
    const graph = scope([connection("first", "a", 0, "b", 0), connection("second", "a", 0, "b", 2)]);
    const original = JSON.stringify(graph);
    const closed = selectedLaneConnections(graph, selected, anchors([group("a", "output")]));
    assert.equal(closed.length, 1);
    assert.deepEqual(closed[0].connectionIds, ["first", "second"]);
    assert.equal(closed[0].target.id, `slot:${group("b", "input")}`);
    const open = selectedLaneConnections(graph, selected, anchors([group("a", "output"), group("b", "input")]));
    assert.equal(open.length, 2);
    assert.equal(new Set(open.map((wire) => wire.source.id)).size, 1);
    assert.equal(new Set(open.map((wire) => wire.target.id)).size, 2);
    assert.equal(JSON.stringify(graph), original);
    assert.equal(selectedLaneConnections(graph, selected, anchors([group("a", "output")])).length, 1);
});

test("distinct visible selected ports keep distinct wires to one collapsed destination", () => {
    const graph = scope([connection("first", "a", 0, "b", 0), connection("second", "a", 1, "b", 2)]);
    const wires = selectedLaneConnections(graph, selected, anchors([group("a", "output")]));
    assert.equal(wires.length, 2);
    assert.equal(new Set(wires.map((wire) => wire.source.id)).size, 2);
    assert.equal(new Set(wires.map((wire) => wire.target.id)).size, 1);
});

test("incoming connections split when the other lane's output slot opens", () => {
    const graph = scope([connection("first", "b", 0, "a", 1), connection("second", "b", 2, "a", 1)]);
    const closed = selectedLaneConnections(graph, selected, anchors([group("a", "input")]));
    assert.equal(closed.length, 1);
    assert.equal(closed[0].source.id, `slot:${group("b", "output")}`);
    const open = selectedLaneConnections(graph, selected, anchors([group("a", "input"), group("b", "output")]));
    assert.equal(open.length, 2);
    assert.equal(new Set(open.map((wire) => wire.source.id)).size, 2);
    assert.equal(new Set(open.map((wire) => wire.target.id)).size, 1);
});

test("wildcard fan-in and fan-out expand according to the visible endpoints", () => {
    const graph = scope([{ id: "many", applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: "a", port: {} }, { instanceId: "a", port: { index: 0 } }],
        inputs: [{ instanceId: "b", port: {} }],
    } }]);
    assert.equal(selectedLaneConnections(graph, selected, anchors([])).length, 1);
    assert.equal(selectedLaneConnections(graph, selected, anchors([group("b", "input")])).length, 3);
    const open = selectedLaneConnections(graph, selected, anchors([group("a", "output"), group("b", "input")]));
    assert.equal(open.length, 9);
    assert.ok(open.every((wire) => wire.connectionIds.length === 1));
});

test("collapsed fan-in and fan-out resolve anchors once per port rather than per logical pair", () => {
    const graph = scope([{ id: "many", applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: "a", port: {} }], inputs: [{ instanceId: "b", port: {} }],
    } }]);
    graph.nodes[0].sampleOutputs = Array.from({ length: 100 }, (_, index) => ({ name: `out${index}`, index, channelType: "mono" }));
    graph.nodes[1].sampleInputs = Array.from({ length: 100 }, (_, index) => ({ name: `in${index}`, index, channelType: "mono" }));
    let calls = 0;
    const collapsed = anchors([]);
    const wires = selectedLaneConnections(graph, selected, (port) => { ++calls; return collapsed(port); });
    assert.equal(wires.length, 1);
    assert.equal(calls, 200);
});

test("unavailable anchors and unresolved interior matchers do not acquire guessed endpoints", () => {
    const graph = scope([connection("known", "a", 0, "b", 0), {
        id: "interior", applied: false, connection: { kind: "sample",
            outputs: [{ instanceId: "a", port: { index: 0 } }],
            inputs: [{ instanceId: "b", path: [{ kind: "virtualNode", sourceIdentity: "inside" }], port: {} }],
        },
    }]);
    assert.deepEqual(selectedLaneConnections(graph, selected, () => null), []);
    const wires = selectedLaneConnections(graph, selected, anchors([]));
    assert.equal(wires.length, 1);
    assert.deepEqual(wires[0].connectionIds, ["known"]);
});

test("collapsed wires retain pending status and event port identity", () => {
    const graph = scope([connection("applied", "a", 0, "b", 0), {
        ...connection("pending", "a", 0, "b", 1), applied: false,
    }, { id: "events", applied: true, connection: { kind: "event",
        outputs: [{ instanceId: "a", port: { index: 0 } }], inputs: [{ instanceId: "b", port: { index: 0 } }],
    } }]);
    const closed = selectedLaneConnections(graph, selected, anchors([]));
    assert.equal(closed.length, 1);
    assert.equal(closed[0].applied, false);
    const open = selectedLaneConnections(graph, selected, anchors([group("a", "output"), group("b", "input")]));
    assert.equal(open.length, 3);
    assert.equal(open.filter((wire) => wire.applied).length, 2);
});

test("a connection chooser separates wires at one physical port without selecting its lane", () => {
    const graph = scope([connection("first", "a", 0, "b", 0), connection("second", "a", 0, "c", 0)]);
    const original = JSON.stringify(graph);
    // The node and port display names can be identical; IDs still distinguish the rows.
    const port: GraphPortEndpoint = { address: { instanceId: "a" }, direction: "output", kind: "sample", index: 0, name: "out0" };
    const revealed = ["first", "second"].map((connectionId) => ({ connectionId, port }));
    const ordinary = anchors([]);
    const withChooser = (endpoint: GraphPortEndpoint, connectionId: string) =>
        portEndpointKey(endpoint) === portEndpointKey(port)
            ? { id: connectionEndKey(connectionId, endpoint), x: 40, y: connectionId === "first" ? 20 : 40 }
            : ordinary(endpoint);
    const wires = selectedLaneConnections(graph, null, withChooser, revealed);
    assert.equal(wires.length, 2);
    assert.equal(new Set(wires.map((wire) => wire.source.id)).size, 2);
    for (const wire of wires) {
        assert.equal(wire.source.id, connectionEndKey(wire.connectionIds[0], port));
    }
    assert.deepEqual(selectedLaneConnections(graph, null, ordinary), []);
    assert.equal(JSON.stringify(graph), original);
});

test("revealing one input end does not reveal the other inputs of a multi-ended record", () => {
    const graph = scope([{ id: "many", applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: "a", port: {} }], inputs: [{ instanceId: "b", port: {} }],
    } }, connection("unrelated", "c", 0, "b", 2)]);
    const port: GraphPortEndpoint = { address: { instanceId: "b" }, direction: "input", kind: "sample", index: 1, name: "in1" };
    const wires = selectedLaneConnections(graph, null, anchors([group("a", "output"), group("b", "input")]),
        [{ connectionId: "many", port }]);
    assert.equal(wires.length, 3);
    assert.ok(wires.every((wire) => wire.target.id === `port:${portEndpointKey(port)}`));
    assert.ok(wires.every((wire) => wire.connectionIds.length === 1 && wire.connectionIds[0] === "many"));
});

test("chooser attachments retain selected-lane wires and detect self-connections without lane selection", () => {
    const graph = scope([connection("self", "a", 0, "a", 0), connection("external", "a", 0, "b", 0),
        connection("selected", "c", 0, "b", 1)]);
    const port: GraphPortEndpoint = { address: { instanceId: "a" }, direction: "output", kind: "sample", index: 0, name: "out0" };
    const revealed = ["self", "external"].map((connectionId) => ({ connectionId, port }));
    const ordinary = anchors([]); // Every physical port is at the same screen height.
    const opened = selectedLaneConnections(graph, addressKey({ instanceId: "c" }), ordinary, revealed);
    assert.deepEqual(opened.flatMap((wire) => wire.connectionIds).sort(), ["external", "selected", "self"]);
    assert.ok(opened.find((wire) => wire.connectionIds.includes("self"))!.selfLoopY !== undefined);
    assert.equal(opened.find((wire) => wire.connectionIds.includes("external"))!.selfLoopY, undefined);
    const closed = selectedLaneConnections(graph, addressKey({ instanceId: "c" }), ordinary);
    assert.deepEqual(closed.flatMap((wire) => wire.connectionIds), ["selected"]);
});

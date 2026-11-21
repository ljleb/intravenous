import test from "node:test";
import assert from "node:assert/strict";
import { addressKey, type GraphScope, type GraphScopeNode, type GraphScopeConnection } from "../src/graphScope";
import { connectionEndKey, LaneConnectionViewport, portEndpointKey, portSlotKey, resolveLaneConnectionEnds,
    selectedLaneConnections, selectedLaneWirePath } from "../src/selectedLaneConnections";
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
const selected = new Set([addressKey({ instanceId: "a" })]);
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
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([]));
    assert.equal(wires.length, 2);
    assert.deepEqual(wires.flatMap((wire) => wire.connectionIds).sort(), ["incoming", "outgoing"]);
    assert.deepEqual(selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set(), anchors([])), []);
    assert.deepEqual(selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set([addressKey({ instanceId: "removed" })]), anchors([])), []);
});

test("each selected lane emphasizes its own connections without doubling a shared cable", () => {
    const graph = scope([connection("a to b", "a", 0, "b", 0), connection("b to c", "b", 0, "c", 0)]);
    const lanes = new Set([...selected, addressKey({ instanceId: "b" })]);
    assert.deepEqual(selectedLaneConnections(resolveLaneConnectionEnds(graph), lanes, anchors([])).map((wire) => [wire.connectionIds[0], wire.emphasized]),
        [["a to b", true], ["b to c", true]]);
});

test("viewport context keeps visible unrelated cables faint and selected cables emphasized", () => {
    const graph = scope([connection("selected", "a", 1, "b", 1),
        connection("context", "b", 1, "c", 1), connection("offscreen", "c", 2, "b", 1)]);
    const direct = anchors([group("a", "output"), group("b", "input"), group("b", "output"),
        group("c", "input"), group("c", "output")]);
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, direct, [], { top: 10, bottom: 30 });
    assert.deepEqual(wires.map((wire) => [wire.connectionIds[0], wire.emphasized]),
        [["selected", true], ["context", false]]);
});

test("visible C++ cable segments stay on screen regardless of selection", () => {
    const graph = scope([
        { ...connection("crosses viewport", "a", 0, "b", 0), sourceOwned: true },
        { ...connection("enters viewport", "a", 0, "c", 0), sourceOwned: true },
        { ...connection("above viewport", "a", 0, "a", 0), sourceOwned: true },
    ]);
    const endpoints = resolveLaneConnectionEnds(graph);
    const anchor = (port: GraphPortEndpoint) => ({
        id: portEndpointKey(port), x: 10, y: { a: 0, b: 100, c: 50 }[port.address.instanceId],
    });
    for (const selection of [new Set<string>(), selected]) {
        const wires = selectedLaneConnections(endpoints, selection, anchor, [], { top: 40, bottom: 60 });
        assert.deepEqual(wires.map((wire) => wire.connectionIds[0]), ["crosses viewport", "enters viewport"]);
        assert.ok(wires.every((wire) => wire.emphasized === selection.has(addressKey({ instanceId: "a" }))));
    }
});

test("lane cable index updates across disjoint jumps and reversals", () => {
    const graph = scope([connection("top", "a", 0, "b", 0), connection("middle", "c", 0, "d", 0),
        connection("spanning", "a", 0, "h", 0), connection("bottom", "f", 0, "g", 0)]);
    const order = ["a", "b", "c", "d", "e", "f", "g", "h"];
    graph.nodes = order.map(node);
    const addresses = (rows: string[]) => rows.map((instanceId) => addressKey({ instanceId }));
    const index = new LaneConnectionViewport(resolveLaneConnectionEnds(graph), addresses(order));
    const ids = (window: LaneConnectionViewport, first: number, last: number) =>
        [...window.inRows(first, last)].map((entry) => entry.id).sort();
    assert.deepEqual(ids(index, 0, 1), ["spanning", "top"]);
    assert.deepEqual(ids(index, 6, 7), ["bottom", "spanning"]);
    assert.deepEqual(ids(index, 2, 3), ["middle", "spanning"]);
    assert.deepEqual(ids(index, 0, 1), ["spanning", "top"]);

    // Collapse the two middle lanes, expand a different child, and jump
    // before any intermediate rows are drawn.
    const updated = ["a", "b", "inner", "e", "f", "g", "h"];
    graph.nodes = updated.map(node);
    graph.connections.push(connection("new child", "inner", 0, "g", 0));
    const rebuilt = new LaneConnectionViewport(resolveLaneConnectionEnds(graph), addresses(updated));
    assert.deepEqual(ids(rebuilt, 5, 6), ["bottom", "new child", "spanning"]);
    assert.deepEqual(ids(rebuilt, 2, 2), ["new child", "spanning"]);
});

test("C++ routes use the module's public inputs as sources and public outputs as targets", () => {
    const graph = scope([
        { ...connection("public input", "a", 0, "b", 0), sourceOwned: true,
            boundarySources: [{ instanceId: "a", port: { name: "in0" } }],
            connection: { kind: "sample", outputs: [], inputs: [{ instanceId: "b", port: { index: 0 } }] } },
        { ...connection("public output", "b", 0, "a", 0), sourceOwned: true,
            boundaryTargets: [{ instanceId: "a", port: { name: "out0" } }],
            connection: { kind: "sample", outputs: [{ instanceId: "b", port: { index: 0 } }], inputs: [] } },
        connection("project cable", "c", 0, "a", 0),
    ]);
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, (port, _, moduleBoundary) => ({
        id: `${port.address.instanceId}:${port.name}:${moduleBoundary ? "boundary" : "lane"}`,
        moduleBoundary, x: 10, y: moduleBoundary ? 20 : 35,
    }));
    assert.deepEqual(wires.map(({ source, target, sourceOwned }) => [source.id, target.id, sourceOwned]), [
        ["a:in0:boundary", "b:in0:lane", true],
        ["b:out0:lane", "a:out0:boundary", true],
        ["c:out0:lane", "a:in0:lane", false],
    ]);
    assert.equal(selectedLaneWirePath(wires[0]), "M 10 20 C 32 20, 32 35, 10 35");
});

test("a C++ cable and a project cable sharing sockets retain separate drawing styles", () => {
    const authored = connection("project", "a", 0, "b", 0);
    const graph = scope([{ ...authored, id: "C++", sourceOwned: true }, authored]);
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([]));
    assert.deepEqual(wires.map(({ connectionIds, sourceOwned }) => [connectionIds, sourceOwned]), [
        [["C++"], true], [["project"], false],
    ]);
});

test("only interior connections use the module boundary sockets", () => {
    const child = { instanceId: "a", path: [{ kind: "tiledChild", name: "child" }] };
    const inside = connection("inside", "a", 0, "a", 0);
    inside.connection.inputs[0] = { ...child, port: { index: 0 } };
    const graph = scope([connection("outside", "a", 0, "b", 0), inside]);
    const member = { ...node("a"), address: child };
    graph.nodes.push(member);
    graph.childScopes = [{ subject: { instanceId: "a" }, nodes: [member], hasBoundary: true }];
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, (port, _, moduleBoundary) => ({
        id: `${portEndpointKey(port)}:${moduleBoundary ? "boundary" : "lane"}`, x: 10, y: 20,
    }));
    assert.deepEqual(wires.map((wire) => [wire.connectionIds[0], wire.source.id.split(":").at(-1)]), [
        ["outside", "lane"], ["inside", "boundary"],
    ]);
});

test("hover expansion splits a wire to a grouped slot into the connected port rows", () => {
    const graph = scope([connection("first", "a", 0, "b", 0), connection("second", "a", 0, "b", 2)]);
    const original = JSON.stringify(graph);
    const closed = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output")]));
    assert.equal(closed.length, 1);
    assert.deepEqual(closed[0].connectionIds, ["first", "second"]);
    assert.equal(closed[0].target.id, `slot:${group("b", "input")}`);
    const open = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output"), group("b", "input")]));
    assert.equal(open.length, 2);
    assert.equal(new Set(open.map((wire) => wire.source.id)).size, 1);
    assert.equal(new Set(open.map((wire) => wire.target.id)).size, 2);
    assert.equal(JSON.stringify(graph), original);
    assert.equal(selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output")])).length, 1);
});

test("distinct visible selected ports keep distinct wires to one collapsed destination", () => {
    const graph = scope([connection("first", "a", 0, "b", 0), connection("second", "a", 1, "b", 2)]);
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output")]));
    assert.equal(wires.length, 2);
    assert.equal(new Set(wires.map((wire) => wire.source.id)).size, 2);
    assert.equal(new Set(wires.map((wire) => wire.target.id)).size, 1);
});

test("incoming connections split when the other lane's output slot opens", () => {
    const graph = scope([connection("first", "b", 0, "a", 1), connection("second", "b", 2, "a", 1)]);
    const closed = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "input")]));
    assert.equal(closed.length, 1);
    assert.equal(closed[0].source.id, `slot:${group("b", "output")}`);
    const open = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "input"), group("b", "output")]));
    assert.equal(open.length, 2);
    assert.equal(new Set(open.map((wire) => wire.source.id)).size, 2);
    assert.equal(new Set(open.map((wire) => wire.target.id)).size, 1);
});

test("wildcard fan-in and fan-out expand according to the visible endpoints", () => {
    const graph = scope([{ id: "many", applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: "a", port: {} }, { instanceId: "a", port: { index: 0 } }],
        inputs: [{ instanceId: "b", port: {} }],
    } }]);
    assert.equal(selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([])).length, 1);
    assert.equal(selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("b", "input")])).length, 3);
    const open = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output"), group("b", "input")]));
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
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, (port) => { ++calls; return collapsed(port); });
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
    assert.deepEqual(selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, () => null), []);
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([]));
    assert.equal(wires.length, 1);
    assert.deepEqual(wires[0].connectionIds, ["known"]);
});

test("collapsed wires retain pending status and event port identity", () => {
    const graph = scope([connection("applied", "a", 0, "b", 0), {
        ...connection("pending", "a", 0, "b", 1), applied: false,
    }, { id: "events", applied: true, connection: { kind: "event",
        outputs: [{ instanceId: "a", port: { index: 0 } }], inputs: [{ instanceId: "b", port: { index: 0 } }],
    } }]);
    const closed = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([]));
    assert.equal(closed.length, 1);
    assert.equal(closed[0].applied, false);
    const open = selectedLaneConnections(resolveLaneConnectionEnds(graph), selected, anchors([group("a", "output"), group("b", "input")]));
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
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set(), withChooser, revealed);
    assert.equal(wires.length, 2);
    assert.equal(new Set(wires.map((wire) => wire.source.id)).size, 2);
    for (const wire of wires) {
        assert.equal(wire.source.id, connectionEndKey(wire.connectionIds[0], port));
        assert.equal(wire.emphasized, false);
    }
    assert.deepEqual(selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set(), ordinary), []);
    assert.equal(JSON.stringify(graph), original);
});

test("revealing one input end does not reveal the other inputs of a multi-ended record", () => {
    const graph = scope([{ id: "many", applied: true, connection: { kind: "sample",
        outputs: [{ instanceId: "a", port: {} }], inputs: [{ instanceId: "b", port: {} }],
    } }, connection("unrelated", "c", 0, "b", 2)]);
    const port: GraphPortEndpoint = { address: { instanceId: "b" }, direction: "input", kind: "sample", index: 1, name: "in1" };
    const wires = selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set(), anchors([group("a", "output"), group("b", "input")]),
        [{ connectionId: "many", port }]);
    assert.equal(wires.length, 3);
    assert.ok(wires.every((wire) => wire.target.id === `port:${portEndpointKey(port)}`));
    assert.ok(wires.every((wire) => wire.connectionIds.length === 1 && wire.connectionIds[0] === "many"));
});

test("chooser attachments retain selected-lane wires and use the same geometry for self-connections", () => {
    const graph = scope([connection("self", "a", 0, "a", 0), connection("external", "a", 0, "b", 0),
        connection("selected", "c", 0, "b", 1)]);
    const port: GraphPortEndpoint = { address: { instanceId: "a" }, direction: "output", kind: "sample", index: 0, name: "out0" };
    const revealed = ["self", "external"].map((connectionId) => ({ connectionId, port }));
    const ordinary = anchors([]); // Every physical port is at the same screen height.
    const opened = selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set([addressKey({ instanceId: "c" })]), ordinary, revealed);
    assert.deepEqual(opened.flatMap((wire) => wire.connectionIds).sort(), ["external", "selected", "self"]);
    assert.deepEqual(opened.filter((wire) => wire.emphasized).flatMap((wire) => wire.connectionIds), ["selected"]);
    const self = opened.find((wire) => wire.connectionIds.includes("self"))!;
    const external = opened.find((wire) => wire.connectionIds.includes("external"))!;
    assert.notEqual(self.key, external.key);
    assert.equal(selectedLaneWirePath(self), selectedLaneWirePath(external));
    const closed = selectedLaneConnections(resolveLaneConnectionEnds(graph), new Set([addressKey({ instanceId: "c" })]), ordinary);
    assert.deepEqual(closed.flatMap((wire) => wire.connectionIds), ["selected"]);
});

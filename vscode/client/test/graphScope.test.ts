import test from "node:test";
import assert from "node:assert/strict";
import { addressKey, placeNodes, readLayout, resolveScopePorts, zoomAt,
    type GraphScope, type GraphScopeNode } from "../src/graphScope";
import { semanticNodeFromScopeChild } from "../webview/presentation/semanticNode";
import { nodeDescription, nodeDisplayName, nodeIdentitySummary } from "../webview/presentation/nodeSummary";
import { nodeDebugDetails } from "../src/nodeDebugDetails";

function node(id: string): GraphScopeNode {
    return {
        address: { instanceId: id, path: [] },
        instance: { instanceId: id, definitionId: "test.filter", realized: true },
        hasChildren: false,
        loadState: "ready",
        sampleInputs: [{ name: "audio", index: 0, channelType: "stereo", valueTypes: [
            { type: "Sample", rank: 0, channelCapability: "audio" },
        ] }],
        sampleOutputs: [{ name: "audio", index: 0, channelType: "stereo" }, { name: "aux", index: 1, channelType: "mono" }],
        eventInputs: [{ name: "gate", index: 0, eventType: "trigger" }], eventOutputs: [],
    };
}
function graph(): GraphScope {
    return { generation: 7, editable: true, nodes: [node("source"), node("sink")], connections: [{
        id: "wire", applied: true, connection: {
            kind: "sample", outputs: [{ instanceId: "source", port: { name: "audio" } }],
            inputs: [{ instanceId: "sink", port: { index: 0 } }],
        },
    }] };
}

test("graph layout keys normalize root addresses and keep instances distinct", () => {
    assert.equal(addressKey({ instanceId: "a" }), addressKey({ instanceId: "a", path: [] }));
    assert.notEqual(addressKey({ instanceId: "a" }), addressKey({ instanceId: "b" }));
    assert.notEqual(addressKey({ instanceId: "a" }), addressKey({ instanceId: "a", path: [{ kind: "tiledChild", name: "left" }] }));
});

test("graph changes preserve moved and temporarily removed node positions", () => {
    const original = graph();
    const saved = placeNodes(original.nodes, readLayout(null));
    saved.positions[addressKey(original.nodes[0].address)] = { x: -930, y: 407 };
    const reduced = placeNodes([original.nodes[1]], saved);
    const restored = placeNodes([...original.nodes, node("new")], reduced);
    assert.deepEqual(restored.positions[addressKey(original.nodes[0].address)], { x: -930, y: 407 });
    assert.deepEqual(restored.positions[addressKey(original.nodes[1].address)], saved.positions[addressKey(original.nodes[1].address)]);
    assert.notDeepEqual(restored.positions[addressKey({ instanceId: "new" })], saved.positions[addressKey(original.nodes[1].address)]);
});

test("default graph placement follows source order without overlapping a saved node", () => {
    const nodes = [node("first"), node("second"), node("third")];
    const saved = readLayout(null);
    saved.positions[addressKey(nodes[2].address)] = { x: 0, y: 0 };
    const placed = placeNodes(nodes, saved).positions;
    assert.ok(placed[addressKey(nodes[0].address)].x > 0);
    assert.ok(placed[addressKey(nodes[1].address)].x > placed[addressKey(nodes[0].address)].x);
    assert.deepEqual(placed[addressKey(nodes[2].address)], { x: 0, y: 0 });
});

test("zoom keeps the graph point under the cursor fixed, including at zoom limits", () => {
    const view = { x: -140, y: 80, scale: 0.8 };
    const cursor = { x: 300, y: 200 };
    for (const requested of [0.001, 0.5, 1.4, 100]) {
        const next = zoomAt(view, cursor, requested);
        assert.ok(next.scale >= 0.1 && next.scale <= 2);
        assert.ok(Math.abs((cursor.x - next.x) / next.scale - (cursor.x - view.x) / view.scale) < 1e-9);
        assert.ok(Math.abs((cursor.y - next.y) / next.scale - (cursor.y - view.y) / view.scale) < 1e-9);
    }
});

test("layout loading rejects invalid geometry while retaining valid positions", () => {
    const layout = readLayout({ positions: { good: { x: -20, y: 80 }, bad: { x: Infinity, y: 2 } },
        viewport: { x: 0, y: 0, scale: 0 } });
    assert.deepEqual(layout.positions, { good: { x: -20, y: 80 } });
    assert.equal(layout.viewport.scale, 1);
});

test("scope port resolution respects kind, direction, all-port matchers, and nested selectors", () => {
    const scope = graph();
    assert.equal(resolveScopePorts(scope, { instanceId: "source", port: {} }, "sample", "output").length, 2);
    assert.equal(resolveScopePorts(scope, { instanceId: "source", port: { name: "audio", index: 1 } }, "sample", "output").length, 0);
    assert.equal(resolveScopePorts(scope, { instanceId: "sink", port: { name: "gate" } }, "event", "input")[0].row, 1);
    assert.equal(resolveScopePorts(scope, { instanceId: "missing", port: {} }, "sample", "input").length, 0);
    assert.equal(resolveScopePorts(scope, { instanceId: "sink", path: [{ kind: "tiledChild", name: "left" }], port: { name: "audio" } }, "sample", "input").length, 0);
});

test("scope presentations use stable addresses, real value types, and desired connection state", () => {
    const scope = graph();
    scope.connections[0].applied = false;
    const sink = semanticNodeFromScopeChild(scope.nodes[1], scope);
    assert.deepEqual(sink.address, scope.nodes[1].address);
    assert.equal(sink.definitionId, "test.filter");
    assert.equal(sink.ports[0].connectivity, "connected");
    assert.equal(sink.ports[1].connectivity, "disconnected");
    assert.equal(sink.ports[0].kind === "continuous" && sink.ports[0].valueTypes[0].type, "Sample");
});

test("default lane names honor user labels without exposing instance identifiers", () => {
    const scope = graph();
    const lane = semanticNodeFromScopeChild(scope.nodes[1], scope);
    assert.equal(nodeDisplayName(lane), "filter");
    assert.equal(nodeIdentitySummary(lane), "");
    assert.equal(nodeDescription(lane), "");
    lane.displayName = "Bass voice";
    assert.equal(nodeDisplayName(lane), "Bass voice");
    assert.equal(lane.definitionId, "test.filter");
    assert.equal(lane.address?.instanceId, "sink");
});

test("pending realization is distinct from a reported configuration failure", () => {
    const scope = graph();
    scope.nodes[1].instance.realized = false;
    scope.nodes[1].loadState = "waitingForDefinition";
    scope.nodes[1].errorMessage = "definition unavailable in the initial snapshot";
    const pending = semanticNodeFromScopeChild(scope.nodes[1], scope);
    assert.equal(pending.realized, false);
    assert.equal(pending.loadState, "waitingForDefinition");
    scope.nodes[1].errorMessage = "Invalid construction argument";
    scope.nodes[1].loadState = "failed";
    assert.equal(semanticNodeFromScopeChild(scope.nodes[1], scope).errorMessage, "Invalid construction argument");
    assert.equal(semanticNodeFromScopeChild(scope.nodes[1], scope).loadState, "failed");
});

test("copied debug details preserve identity, diagnostics, arbitrary ports, and interior connections", () => {
    const scope = graph();
    const sink = scope.nodes[1];
    sink.errorMessage = "configuration failed";
    sink.eventInputs = Array.from({ length: 100 }, (_, index) => ({ name: `event${index}`, index, eventType: "trigger" }));
    scope.connections.push({ id: "interior", applied: false, connection: {
        kind: "event", outputs: [{ instanceId: "source", port: {} }],
        inputs: [{ instanceId: "sink", path: [{ kind: "tiledChild", name: "left" }], port: { index: 33 } }],
    } });
    scope.connections.push({ id: "unrelated", applied: false, connection: {
        kind: "event", outputs: [{ instanceId: "other", port: {} }], inputs: [],
    } });
    const copied = JSON.parse(nodeDebugDetails(scope, sink));
    assert.deepEqual(copied.address, sink.address);
    assert.equal(copied.generation, scope.generation);
    assert.equal(copied.instance.definitionId, "test.filter");
    assert.equal(copied.errorMessage, "configuration failed");
    assert.equal(copied.eventInputs.length, 100);
    assert.deepEqual(copied.connections.map((entry) => entry.id), ["wire", "interior"]);
    assert.equal(copied.connections[1].connection.inputs[0].path[0].name, "left");
    assert.equal(semanticNodeFromScopeChild(sink, scope).ports.filter((port) => port.kind === "event").length, 100);
});

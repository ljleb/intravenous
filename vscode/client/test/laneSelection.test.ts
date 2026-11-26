import test from "node:test";
import assert from "node:assert/strict";
import { addressKey, graphScopeWithChildren, type GraphScope, type GraphScopeNode, type NodeAddress } from "../src/graphScope";
import { graphEditorShortcut } from "../src/graphEditorShortcuts";
import { dropSelectedLanes, expandedGraphLanes, insertLanes, mergeLaneOrder, moveLaneNesting, moveSelectedLanes, moveSelectedSiblingLanes, orderLanes,
    selectedProjectLaneIds, selectLaneRange, laneRuns } from "../src/laneSelection";

test("Shift-click extends or shrinks an inclusive range from the same lane", () => {
    const order = ["a", "b", "c", "d", "e"];
    const start = selectLaneRange(new Set(), order, null, "d");
    const extended = selectLaneRange(start.selected, order, start.anchor, "b", { shiftKey: true });
    assert.deepEqual([...extended.selected], ["b", "c", "d"]);
    const shrunk = selectLaneRange(extended.selected, order, extended.anchor, "c", { shiftKey: true });
    assert.deepEqual([...shrunk.selected], ["c", "d"]);
    const added = selectLaneRange(shrunk.selected, order, shrunk.anchor, "e", { shiftKey: true, ctrlKey: true });
    assert.deepEqual([...added.selected], ["c", "d", "e"]);
    const toggled = selectLaneRange(added.selected, order, added.anchor, "d", { ctrlKey: true });
    assert.deepEqual([...toggled.selected], ["c", "e"]);
    assert.deepEqual([...selectLaneRange(new Set(), order, "removed", "a", { shiftKey: true }).selected], ["a"]);
});

test("moving lane runs preserves both relative orders and stops at the boundary", () => {
    const order = ["a", "b", "c", "d", "e", "f"];
    const selected = new Set(["a", "c", "d", "f"]);
    assert.deepEqual(moveSelectedLanes(order, selected, -1), ["a", "c", "d", "b", "f", "e"]);
    assert.deepEqual(moveSelectedLanes(order, selected, 1), ["b", "a", "e", "c", "d", "f"]);
    assert.equal(moveSelectedLanes(order, new Set(["a", "b"]), -1), order);
    assert.equal(moveSelectedLanes(order, new Set(["e", "f"]), 1), order);
    assert.deepEqual(laneRuns(order, selected), [{ begin: 0, end: 1 }, { begin: 2, end: 4 }, { begin: 5, end: 6 }]);
});

test("the displayed order keeps saved lanes and appends newly visible lanes", () => {
    const lanes = ["new", "above", "below"].map((instanceId) => ({ address: { instanceId } }));
    const [newLane, above, below] = lanes.map(({ address }) => addressKey(address));
    const saved = [above, "temporarily absent", below];
    const merged = mergeLaneOrder(saved, [below, above, newLane]);
    assert.deepEqual(merged, [below, "temporarily absent", above, newLane]);
    assert.deepEqual(orderLanes(lanes, merged).map(({ address }) => addressKey(address)), [below, above, newLane]);
});

test("each duplicate follows its original even when selected lanes are separated", () => {
    const copies = [{ key: "a-copy", after: "a" }, { key: "c-copy", after: "c" }];
    const lanes = ["a", "b", "c", "d"];
    const placed = ["a", "a-copy", "b", "c", "c-copy", "d"];
    assert.deepEqual(insertLanes(lanes, copies), placed);
    assert.deepEqual(insertLanes([...lanes, "a-copy", "c-copy"], copies), placed);
    assert.deepEqual(insertLanes(lanes, []), lanes);
});

test("expanded module children follow their parents and cannot be duplicated on their own", () => {
    const node = (address: NodeAddress, hasChildren = false): GraphScopeNode => ({
        address, instance: { instanceId: address.instanceId, definitionId: "test.node", realized: true },
        loadState: "ready", hasChildren, sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [],
    });
    const module = node({ instanceId: "module" }, true);
    module.isModule = true;
    const child = node({ instanceId: "module", path: [
        { kind: "symbolicNode", sourceIdentity: "voice" }, { kind: "symbolicMember", index: 0 },
    ] }, true);
    const tile = node({ instanceId: "module", path: [...child.address.path!, { kind: "tiledChild", name: "right" }] });
    const sink = node({ instanceId: "sink" });
    const scope: GraphScope = { generation: 1, editable: true, nodes: [module, sink], connections: [],
        childScopes: [{ subject: module.address, nodes: [child] }, { subject: child.address, nodes: [tile] }] };
    const order = [addressKey(sink.address), addressKey(module.address)];
    const childKey = addressKey(child.address);
    const tileKey = addressKey(tile.address);
    const expanded = expandedGraphLanes(scope, order, new Set([order[1], childKey]));
    assert.deepEqual(expanded.map(({ node, depth }) => [node, depth]), [
        [sink, 0], [module, 0], [child, 1], [tile, 2],
    ]);
    assert.deepEqual(selectedProjectLaneIds(expanded, new Set([childKey, tileKey])), []);
    assert.deepEqual(selectedProjectLaneIds(expanded, new Set([order[1], childKey, tileKey])), ["module"]);
    assert.deepEqual(expandedGraphLanes(scope, order, new Set([childKey])), expanded.slice(0, 2));
    const alias = node({ instanceId: "module", path: [{ kind: "symbolicNode", sourceIdentity: "alias" }] }, true);
    const otherRow = { ...tile, membershipAddress: { instanceId: "module", path: [
        ...alias.address.path!, { kind: "symbolicMember", index: 0 },
    ] } };
    scope.childScopes![0].nodes.push(alias);
    scope.childScopes!.push({ subject: alias.address, nodes: [otherRow] });
    const withAliases = expandedGraphLanes(scope, order, new Set([order[1], childKey, addressKey(alias.address)]));
    const memberRows = withAliases.filter(({ node }) => addressKey(node.address) === tileKey);
    assert.equal(memberRows.length, 2);
    assert.notEqual(memberRows[0].rowKey, memberRows[1].rowKey);
    const laneKeys = withAliases.map(({ node }) => addressKey(node.address));
    for (const row of memberRows) {
        const click = selectLaneRange(new Set(), laneKeys, null, addressKey(row.node.address));
        assert.deepEqual(withAliases.filter(({ node }) => click.selected.has(addressKey(node.address))), memberRows);
    }
    assert.equal(graphScopeWithChildren(scope).nodes.filter(({ address }) => addressKey(address) === tileKey).length, 1);

    child.isModule = true;
    const hosted = { [order[0]]: childKey };
    const hostedRows = expandedGraphLanes(scope, order, new Set([order[1], childKey]), hosted);
    assert.deepEqual(hostedRows.filter(({ node }) => node === sink).map(({ depth }) => depth), [2]);
    const out = moveLaneNesting(hostedRows, order, hosted, new Set([order[0]]), -1)!;
    assert.deepEqual(out.parents, { [order[0]]: order[1] });
    assert.deepEqual(expandedGraphLanes(scope, out.order, new Set([order[1], childKey]), out.parents)
        .filter(({ node }) => node === sink).map(({ depth }) => depth), [1]);
});

test("adding a lane uses its selected anchor or appends if no anchor remains", () => {
    const lanes = ["a", "b", "c"];
    assert.deepEqual(insertLanes(lanes, [{ key: "new", after: "b" }]), ["a", "b", "new", "c"]);
    assert.deepEqual(insertLanes(lanes, [{ key: "new" }]), ["a", "b", "c", "new"]);
    assert.deepEqual(insertLanes(lanes, [{ key: "new", after: "deleted" }]), ["a", "b", "c", "new"]);
});

test("Shift+Up and Shift+Down move a project lane across source-owned siblings", () => {
    const node = (address: NodeAddress): GraphScopeNode => ({
        address, instance: { instanceId: address.instanceId, definitionId: "test", realized: true },
        loadState: "ready", hasChildren: true,
        sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [],
    });
    const module = { ...node({ instanceId: "module" }), isModule: true };
    const curve = node({ instanceId: "curve" });
    const first = node({ instanceId: "module", path: [{ kind: "symbolicNode", sourceIdentity: "first" }] });
    const second = node({ instanceId: "module", path: [{ kind: "symbolicNode", sourceIdentity: "second" }] });
    const member = node({ instanceId: "module", path: [...first.address.path!, { kind: "symbolicMember", index: 0 }] });
    const scope: GraphScope = { generation: 1, editable: true, nodes: [module, curve], connections: [],
        childScopes: [{ subject: module.address, nodes: [first, second] }, { subject: first.address, nodes: [member] }] };
    const [m, p, a, b, c] = [module, curve, first, second, member].map(({ address }) => addressKey(address));
    const expanded = new Set([m, a]);
    const parents = { [p]: m };
    const start = [m, p];
    const rows = (order: readonly string[], open = expanded) => expandedGraphLanes(scope, order, open, parents);
    const keys = (order: readonly string[]) => rows(order).map(({ node }) => addressKey(node.address));
    assert.deepEqual(keys(start), [m, a, c, b, p]);

    const selected = new Set([p, b]);
    const up = moveSelectedSiblingLanes(rows(start), start, selected, -1);
    assert.deepEqual(keys(up), [m, a, c, p, b]);
    assert.deepEqual(rows(up, new Set()).map(({ node }) => addressKey(node.address)), [m]);
    const top = moveSelectedSiblingLanes(rows(up), up, selected, -1);
    assert.deepEqual(keys(top), [m, p, a, c, b]);
    assert.equal(moveSelectedSiblingLanes(rows(top), top, selected, -1), top);
    assert.deepEqual(keys(moveSelectedSiblingLanes(rows(top), top, selected, 1)), [m, a, c, p, b]);

    const copy = node({ instanceId: "copy" });
    const copyKey = addressKey(copy.address);
    const withCopy = expandedGraphLanes({ ...scope, nodes: [...scope.nodes, copy] },
        insertLanes(up, [{ key: copyKey, after: p }]), expanded, { ...parents, [copyKey]: m });
    assert.deepEqual(withCopy.map(({ node }) => addressKey(node.address)), [m, a, c, p, copyKey, b]);

    const aliasRows = [first, copy, second].map((node) => ({
        node, depth: 1, rowKey: "alias/" + addressKey(node.address), parentRowKey: "alias",
    }));
    const withAlias = moveSelectedSiblingLanes([...rows(up), ...aliasRows],
        insertLanes(up, [{ key: copyKey, after: p }]), new Set([copyKey]), -1);
    assert.deepEqual(keys(withAlias), keys(up));
    assert.ok(withAlias.indexOf(copyKey) < withAlias.indexOf(a));
});

test("project lanes nest per sibling run and move one level out", () => {
    const nodes: GraphScopeNode[] = ["m", "a", "b", "n", "c", "d"].map((instanceId) => ({
        address: { instanceId }, instance: { instanceId, definitionId: "test", realized: true },
        loadState: "ready", hasChildren: false,
        sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [],
    }));
    const [m, a, b, n, c, d] = nodes.map(({ address }) => addressKey(address));
    const scope: GraphScope = { generation: 1, editable: true, nodes, connections: [] };
    const order = [m, a, b, n, c, d];
    const expanded = new Set([m, n]);
    const before = expandedGraphLanes(scope, order, expanded);
    const selected = new Set([a, b, c]);
    assert.equal(moveLaneNesting(before, order, {}, selected, -1), undefined);
    const nested = moveLaneNesting(before, order, {}, selected, 1)!;
    assert.deepEqual(nested.parents, { [a]: m, [b]: m, [c]: n });
    const after = expandedGraphLanes(scope, nested.order, expanded, nested.parents);
    assert.deepEqual(after.map(({ depth }) => depth), [0, 1, 1, 0, 1, 0]);
    assert.deepEqual(after.map(({ node }) => addressKey(node.address)), order);

    const reordered = moveSelectedSiblingLanes(after, nested.order, new Set([a]), 1);
    assert.deepEqual(expandedGraphLanes(scope, reordered, expanded, nested.parents)
        .map(({ node }) => addressKey(node.address)), [m, b, a, n, c, d]);
    const out = moveLaneNesting(after, nested.order, nested.parents, new Set([m, d, c]), -1)!;
    assert.deepEqual(out.parents, { [a]: m, [b]: m });
    assert.deepEqual(expandedGraphLanes(scope, out.order, expanded, out.parents)
        .map(({ node }) => addressKey(node.address)), order);

    assert.equal(dropSelectedLanes(after, nested.order, nested.parents, new Set([m]), { parent: after[1] }), undefined);
    const dropped = dropSelectedLanes(after, nested.order, nested.parents, new Set([a, c]), { parent: after[3] })!;
    assert.deepEqual(dropped.parents, { [a]: n, [b]: m, [c]: n });
    assert.deepEqual(expandedGraphLanes(scope, dropped.order, new Set([m]), dropped.parents)
        .map(({ node }) => addressKey(node.address)), [m, b, n, d]);
    const unavailable = { ...scope, laneParentFallbacks: [{ requested: nodes[3].address, effective: null }] };
    assert.deepEqual(expandedGraphLanes(unavailable, dropped.order, new Set([m]), dropped.parents)
        .map(({ node }) => addressKey(node.address)), [m, b, n, d, a, c]);
});

test("lane parent fallback preserves the saved occurrence through collapse and restoration", () => {
    const root = { instanceId: "module", path: [] };
    const symbol = { instanceId: "module", path: [{ kind: "symbolicNode", sourceIdentity: "first", typeIdentity: "Leaf" }] };
    const alias = { ...symbol, path: [{ ...symbol.path[0], sourceIdentity: "second" }] };
    const member = { ...root, path: [...symbol.path, { kind: "symbolicMember", index: 29 }] };
    const aliasMember = { ...root, path: [...alias.path, { kind: "symbolicMember", index: 29 }] };
    const project = { instanceId: "automation" };
    const node = (address: GraphScopeNode["address"], membershipAddress?: GraphScopeNode["membershipAddress"]): GraphScopeNode => ({
        address, membershipAddress, instance: { instanceId: address.instanceId, definitionId: "test", realized: true },
        loadState: "ready", hasChildren: false, sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [],
    });
    const [r, s, a, m, am, p] = [root, symbol, alias, member, aliasMember, project].map(addressKey);
    const scope: GraphScope = {
        generation: 1, editable: true, nodes: [node(root), node(project)], connections: [],
        presentation: {
            laneOrder: [root, project],
            laneParents: [{ instanceId: project.instanceId, parent: aliasMember }],
            laneViews: [],
            timelineSnapMemory: {},
        },
        childScopes: [
            { subject: root, nodes: [node(symbol), node(alias)] },
            { subject: symbol, nodes: [node(member)] },
            { subject: alias, nodes: [node(member, aliasMember)] },
        ],
    };
    const visible = (scope: GraphScope, ...expanded: string[]) => expandedGraphLanes(scope, [r, p], new Set(expanded));
    const restored = visible(scope, r, s, a, m, am);
    assert.deepEqual(restored.map(({ rowKey }) => rowKey), [r, s, m, a, am, p]);
    assert.equal(restored.find(({ rowKey }) => rowKey === m)?.hasProjectChildren, false);
    assert.equal(restored.find(({ rowKey }) => rowKey === am)?.hasProjectChildren, true);
    assert.equal(restored.at(-1)?.parentRowKey, am);
    const shrunk = {
        ...scope,
        childScopes: scope.childScopes!.slice(0, 1),
        laneParentFallbacks: [{ requested: aliasMember, effective: alias }],
    };
    assert.deepEqual(visible(shrunk, r).map(({ rowKey }) => rowKey), [r, s, a]);
    const fallback = visible(shrunk, r, a);
    assert.equal(fallback.at(-1)?.parentRowKey, a);
    assert.equal(dropSelectedLanes(restored, [r, p], { [p]: am }, new Set([r]),
        { parent: restored.find((lane) => lane.rowKey === am) }), undefined);
    const out = moveLaneNesting(restored, [r, p], { [p]: am }, new Set([p]), -1)!;
    assert.deepEqual(out.parents, { [p]: a });
    assert.equal(out.order[out.order.indexOf(am) + 1], p);
    assert.equal(visible(scope, r, a, am).at(-1)?.parentRowKey, am);
});

test("dropping beside a source child keeps a saved fallback path and source sibling order", () => {
    const root = { instanceId: "root" };
    const first = { ...root, path: [{ kind: "symbolicNode", sourceIdentity: "first" }] };
    const second = { ...root, path: [{ kind: "symbolicNode", sourceIdentity: "second" }] };
    const missing = { ...second, path: [...second.path, { kind: "symbolicMember", index: 29 }] };
    const child = { instanceId: "child" };
    const node = (address: GraphScopeNode["address"]): GraphScopeNode => ({
        address, instance: { instanceId: address.instanceId, definitionId: "test", realized: true },
        loadState: "ready", hasChildren: false, sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [],
    });
    const [r, a, b, m, c] = [root, first, second, missing, child].map(addressKey);
    const scope: GraphScope = {
        generation: 1, editable: true, nodes: [node(root), node(child)], connections: [],
        childScopes: [{ subject: root, nodes: [node(first), node(second)] }],
        laneParentFallbacks: [{ requested: missing, effective: root }],
    };
    const parents = { [c]: m };
    const expanded = new Set([r]);
    const initial = expandedGraphLanes(scope, [r, c], expanded, parents);
    const drop = dropSelectedLanes(initial, [r, c], parents, new Set([c]), { parent: initial[0], before: b })!;
    assert.deepEqual(drop.parents, parents);
    assert.deepEqual(expandedGraphLanes(scope, drop.order, expanded, parents).map(({ rowKey }) => rowKey), [r, a, c, b]);
});

test("graph shortcuts distinguish add, duplicate, and select all", () => {
    const key = { key: "a", ctrlKey: false, metaKey: false, altKey: false, shiftKey: false };
    assert.equal(graphEditorShortcut({ ...key, key: "A", shiftKey: true }), "addNode");
    assert.equal(graphEditorShortcut({ ...key, ctrlKey: true }), "selectAll");
    assert.equal(graphEditorShortcut({ ...key, key: "d", ctrlKey: true }), "duplicateLanes");
    assert.equal(graphEditorShortcut({ ...key, key: "d", metaKey: true }), "duplicateLanes");
    assert.equal(graphEditorShortcut({ ...key, ctrlKey: true, shiftKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, ctrlKey: true, isComposing: true }), null);
});

test("Shift+R resets the graph without changing native keyboard shortcuts", () => {
    const key = { key: "R", ctrlKey: false, metaKey: false, altKey: false, shiftKey: true };
    assert.equal(graphEditorShortcut(key), "resetGraphState");
    assert.equal(graphEditorShortcut({ ...key, shiftKey: false }), null);
    assert.equal(graphEditorShortcut({ ...key, ctrlKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, metaKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, altKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, isComposing: true }), null);
});

test("knob clipboard shortcuts leave native clipboard shortcuts and composition alone", () => {
    const key = { key: "c", ctrlKey: false, metaKey: false, altKey: false, shiftKey: false };
    assert.equal(graphEditorShortcut(key), "copyKnobValue");
    assert.equal(graphEditorShortcut({ ...key, key: "v" }), "pasteKnobValue");
    assert.equal(graphEditorShortcut({ ...key, ctrlKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, key: "v", metaKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, key: "V", shiftKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, altKey: true }), null);
    assert.equal(graphEditorShortcut({ ...key, isComposing: true }), null);
});

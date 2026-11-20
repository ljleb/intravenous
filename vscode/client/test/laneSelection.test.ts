import test from "node:test";
import assert from "node:assert/strict";
import { addressKey } from "../src/graphScope";
import { graphEditorShortcut } from "../src/graphEditorShortcuts";
import { insertLanes, mergeLaneOrder, moveSelectedLanes, orderLanes, selectLaneRange, laneRuns } from "../src/laneSelection";

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

test("adding a lane uses its selected anchor or appends if no anchor remains", () => {
    const lanes = ["a", "b", "c"];
    assert.deepEqual(insertLanes(lanes, [{ key: "new", after: "b" }]), ["a", "b", "new", "c"]);
    assert.deepEqual(insertLanes(lanes, [{ key: "new" }]), ["a", "b", "c", "new"]);
    assert.deepEqual(insertLanes(lanes, [{ key: "new", after: "deleted" }]), ["a", "b", "c", "new"]);
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

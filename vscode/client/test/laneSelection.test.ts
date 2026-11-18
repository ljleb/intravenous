import test from "node:test";
import assert from "node:assert/strict";
import { addressKey } from "../src/graphScope";
import { mergeLaneOrder, moveSelectedLanes, orderLanes, selectLaneRange, selectedLaneRuns } from "../src/laneSelection";

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
    assert.deepEqual(selectedLaneRuns(order, selected), [{ begin: 0, end: 1 }, { begin: 2, end: 4 }, { begin: 5, end: 6 }]);
});

test("the displayed order keeps saved lanes and appends newly visible lanes", () => {
    const lanes = ["new", "above", "below"].map((instanceId) => ({ address: { instanceId } }));
    const [newLane, above, below] = lanes.map(({ address }) => addressKey(address));
    const saved = [above, "temporarily absent", below];
    const merged = mergeLaneOrder(saved, [below, above, newLane]);
    assert.deepEqual(merged, [below, "temporarily absent", above, newLane]);
    assert.deepEqual(orderLanes(lanes, merged).map(({ address }) => addressKey(address)), [below, above, newLane]);
});

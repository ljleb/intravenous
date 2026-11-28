import test from "node:test";
import assert from "node:assert/strict";
import { projectHistoryShortcut } from "../src/projectHistoryShortcut";

const key = { key: "z", ctrlKey: true, metaKey: false, altKey: false, shiftKey: false };
test("graph history shortcuts support Ctrl or Command undo and redo", () => {
    assert.equal(projectHistoryShortcut(key, false), "undo");
    assert.equal(projectHistoryShortcut({ ...key, key: "Z", shiftKey: true }, false), "redo");
    assert.equal(projectHistoryShortcut({ ...key, key: "y" }, false), "redo");
    assert.equal(projectHistoryShortcut({ ...key, ctrlKey: false, metaKey: true }, false), "undo");
    assert.equal(projectHistoryShortcut({ ...key, ctrlKey: false, metaKey: true, shiftKey: true }, false), "redo");
});
test("text editing, composition, handled events, and unrelated keys retain their own behaviour", () => {
    assert.equal(projectHistoryShortcut(key, true), null);
    assert.equal(projectHistoryShortcut({ ...key, isComposing: true }, false), null);
    assert.equal(projectHistoryShortcut({ ...key, defaultPrevented: true }, false), null);
    assert.equal(projectHistoryShortcut({ ...key, altKey: true }, false), null);
    assert.equal(projectHistoryShortcut({ ...key, ctrlKey: false }, false), null);
    assert.equal(projectHistoryShortcut({ ...key, key: "a" }, false), null);
});

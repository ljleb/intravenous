import test from "node:test";
import assert from "node:assert/strict";

import {
    compactWidthPx,
    effectiveDisplayMode,
    isNodeViewChoice,
} from "../webview/presentation/displayMode";
import { PresentationRegistry } from "../webview/presentation/registry";

function registry(): PresentationRegistry<string> {
    const presentations = new PresentationRegistry<string>({
        presentationId: "iv.generic",
        displayName: "Generic",
        component: "generic",
    });
    presentations.register({
        presentationId: "iv.test.voice.scope",
        displayName: "Scope",
        definitionIds: ["iv.test.voice", "iv.test.osc"],
        component: "scope",
    });
    presentations.register({
        presentationId: "iv.test.voice.controls",
        displayName: "Controls",
        definitionIds: ["iv.test.voice"],
        component: "controls",
    });
    return presentations;
}

test("an unregistered definition uses the generic presentation", () => {
    const selection = registry().select("iv.test.unknown");
    assert.equal(selection.component, "generic");
    assert.equal(selection.usedGeneric, true);
    assert.equal(registry().select("").component, "generic");
});

test("without a preference the first candidate by presentation ID is selected", () => {
    const presentations = registry();
    assert.deepEqual(
        presentations.candidates("iv.test.voice").map((candidate) => candidate.presentationId),
        ["iv.test.voice.controls", "iv.test.voice.scope"]);
    assert.equal(presentations.select("iv.test.voice").component, "controls");
    assert.equal(presentations.select("iv.test.osc").component, "scope");
});

test("an available preference wins and an unavailable one is ignored", () => {
    const presentations = registry();
    assert.equal(presentations.select("iv.test.voice", "iv.test.voice.scope").component, "scope");
    assert.equal(presentations.select("iv.test.osc", "iv.test.voice.controls").component, "scope");
    assert.equal(presentations.select("iv.test.voice", "iv.generic").component, "generic");
});

test("presentation IDs are unique and must name definitions", () => {
    const presentations = registry();
    assert.throws(() => presentations.register({
        presentationId: "iv.test.voice.scope",
        displayName: "Again",
        definitionIds: ["iv.test.voice"],
        component: "again",
    }));
    assert.throws(() => presentations.register({
        presentationId: "iv.generic",
        displayName: "Generic",
        definitionIds: ["iv.test.voice"],
        component: "generic",
    }));
    assert.throws(() => presentations.register({
        presentationId: "iv.test.empty",
        displayName: "Empty",
        definitionIds: [],
        component: "empty",
    }));
});

test("the host imposes compact below the normal width, with hysteresis", () => {
    assert.equal(effectiveDisplayMode(null, "editable", null), "editable");
    assert.equal(effectiveDisplayMode(compactWidthPx + 1, "nano", null), "nano");
    assert.equal(effectiveDisplayMode(compactWidthPx - 1, "nano", "nano"), "compact");
    // Leaving compact requires more width than entering it.
    assert.equal(effectiveDisplayMode(compactWidthPx + 1, "mini", "compact"), "compact");
    assert.equal(effectiveDisplayMode(compactWidthPx + 64, "mini", "compact"), "mini");
});

test("compact is not a view choice", () => {
    assert.equal(isNodeViewChoice("mini"), true);
    assert.equal(isNodeViewChoice("compact"), false);
    assert.equal(isNodeViewChoice(undefined), false);
});

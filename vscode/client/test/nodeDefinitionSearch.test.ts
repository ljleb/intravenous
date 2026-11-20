import assert from "node:assert/strict";
import { test } from "node:test";
import { applyTagCompletion, queryByteOffset, rankNodeDefinitions, type NodeDefinitionPreview } from "../src/nodeDefinitionSearch";

const node = (id: string, displayName: string): NodeDefinitionPreview => ({
    id, displayName, package: "builtin", tags: [], ports: [], requiresConfiguration: false,
});

test("node picker uses relevance before add frequency and ranks exact tag matches by frequency", () => {
    const curve = node("builtin.automation.curve", "Automation Curve");
    const envelope = node("example.automation.envelope", "Automation Envelope");
    const definitions = [curve, envelope];
    const usage = { [envelope.id]: 20 };
    const editor = node("builtin.automation.curve_editor", "Curve Editor");

    assert.deepEqual(rankNodeDefinitions(definitions, "automation curve", undefined, usage), [curve]);
    assert.deepEqual(rankNodeDefinitions([curve, editor], "automation curve", undefined, { [editor.id]: 100 }), [curve, editor]);
    assert.deepEqual(rankNodeDefinitions(definitions, "auto", undefined, usage), [envelope, curve]);
    assert.deepEqual(rankNodeDefinitions(definitions, "automation", definitions.map((entry) => entry.id), usage),
        [envelope, curve]);
    assert.deepEqual(rankNodeDefinitions(definitions, "automation !curve", [], usage), []);
});

test("tag completion replaces the UTF-8 range and keeps the query around it", () => {
    const source = "é effect.sat !experimental";
    const start = queryByteOffset(source, "é effect.".length);
    const end = queryByteOffset(source, "é effect.sat".length);
    assert.deepEqual(applyTagCompletion(source, start, end, "saturation"), {
        source: "é effect.saturation !experimental", caret: "é effect.saturation".length,
    });
});

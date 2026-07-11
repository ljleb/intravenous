import test from "node:test";
import assert from "node:assert/strict";

import { sortNodesByRelevance, collectPrimarySourceSpans } from "../src/graphQueryModel";
import { LogicalNode } from "../src/graphModel";

test("sortNodesByRelevance prefers the closest span in the earliest query range", () => {
    const nodes: LogicalNode[] = [
        {
            id: "far",
            sourceSpans: [{
                filePath: "/tmp/module.cpp",
                start: { line: 20, column: 1 },
                end: { line: 20, column: 5 },
            }],
        },
        {
            id: "near",
            sourceSpans: [{
                filePath: "/tmp/module.cpp",
                start: { line: 10, column: 2 },
                end: { line: 10, column: 6 },
            }],
        },
    ];

    const sorted = sortNodesByRelevance(nodes, {
        filePath: "/tmp/module.cpp",
        ranges: [{
            start: { line: 10, column: 1 },
            end: { line: 10, column: 7 },
        }],
    });

    assert.deepEqual(sorted.map((node) => node.id), ["near", "far"]);
});

test("collectPrimarySourceSpans removes duplicate spans while preserving order", () => {
    const spans = collectPrimarySourceSpans([{
        id: "node-a",
        sourceSpans: [
            {
                filePath: "/tmp/module.cpp",
                start: { line: 1, column: 1 },
                end: { line: 1, column: 5 },
            },
            {
                filePath: "/tmp/module.cpp",
                start: { line: 1, column: 1 },
                end: { line: 1, column: 5 },
            },
        ],
    }]);

    assert.equal(spans.length, 1);
    assert.equal(spans[0].filePath, "/tmp/module.cpp");
});

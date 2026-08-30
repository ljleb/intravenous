import test from "node:test";
import assert from "node:assert/strict";

import { formatTimestampedLogEntry, timestampOutput } from "../src/outputLog";

test("formatTimestampedLogEntry prepends an ISO timestamp in brackets", () => {
    assert.equal(
        formatTimestampedLogEntry("Module build ready to apply", new Date("2026-08-30T12:34:56.789Z")),
        "[2026-08-30T12:34:56.789Z] Module build ready to apply",
    );
});

test("timestampOutput timestamps every appended line", () => {
    const lines: string[] = [];
    const output = timestampOutput(
        { appendLine: (message: string) => lines.push(message) },
        () => new Date("2026-08-30T12:34:56.789Z"),
    );

    output.appendLine("first");
    output.appendLine("second");

    assert.deepEqual(lines, [
        "[2026-08-30T12:34:56.789Z] first",
        "[2026-08-30T12:34:56.789Z] second",
    ]);
});

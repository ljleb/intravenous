import test from "node:test";
import assert from "node:assert/strict";

import { autoDetectedServerDirectoriesForWorkspaceRoot } from "../src/serverBinaryPaths";

test("workspaceSession server auto-detection includes parent repo build directories", () => {
    const workspaceRoot = "/home/abstrack/src/intravenous/projects/simple_sine";
    const directories = autoDetectedServerDirectoriesForWorkspaceRoot(workspaceRoot)
        .map((candidate) => candidate.directory);

    assert.ok(
        directories.includes("/home/abstrack/src/intravenous/build/src/intravenous"),
        "expected repo-level build/src/intravenous to be searched",
    );
    assert.ok(
        directories.includes("/home/abstrack/src/intravenous/build/intravenous"),
        "expected repo-level build/intravenous to be searched",
    );
    assert.equal(directories[0], "/home/abstrack/src/intravenous/projects/simple_sine/build/src/intravenous");
    assert.equal(directories[1], "/home/abstrack/src/intravenous/projects/simple_sine/build/intravenous");
});

import test from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { knobPointerTraceDirectory } from "../src/knobPointerTraceFile";

test("pointer reports use the repository's debug-release directory for nested projects and worktrees", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "iv-pointer-trace-"));
    try {
        const project = path.join(root, "projects", "simple_sine");
        fs.mkdirSync(project, { recursive: true });
        fs.writeFileSync(path.join(root, ".git"), "gitdir: /tmp/worktree-metadata\n");
        assert.equal(knobPointerTraceDirectory(project), path.join(root, "debug-release"));
    } finally { fs.rmSync(root, { recursive: true, force: true }); }
});

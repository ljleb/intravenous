import test from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs/promises";
import * as path from "node:path";
import * as os from "node:os";
import { GraphLayoutStore } from "../src/graphLayoutStore";
import { readLayout } from "../src/graphScope";

test("graph layout saves serialize, survive a new store, and leave project intent untouched", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const project = path.join(directory, "iv_project.jsonl");
        await fs.writeFile(project, "project intent\n");
        const store = new GraphLayoutStore(directory);
        assert.deepEqual(await store.load(), readLayout(null));
        const first = readLayout({ positions: { node: { x: 2, y: 3 } } });
        const last = readLayout({ positions: { node: { x: 200, y: -300 } }, viewport: { x: 10, y: 20, scale: 0.5 } });
        await Promise.all([store.save(first), store.save(last)]);
        assert.deepEqual(await new GraphLayoutStore(directory).load(), last);
        assert.equal(await fs.readFile(project, "utf8"), "project intent\n");
        assert.deepEqual(await fs.readdir(path.dirname(store.filePath)), ["graph-layout.json"]);
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test("an unreadable layout is reported instead of silently replacing user data", async () => {
    const directory = await fs.mkdtemp(path.join(os.tmpdir(), "iv-graph-layout-"));
    try {
        const store = new GraphLayoutStore(directory);
        await fs.mkdir(path.dirname(store.filePath));
        await fs.writeFile(store.filePath, "{broken");
        await assert.rejects(store.load(), SyntaxError);
        assert.equal(await fs.readFile(store.filePath, "utf8"), "{broken");
    } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

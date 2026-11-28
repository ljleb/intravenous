import test from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";

import { resolveIntravenousBinary } from "../src/serverBinaryPaths";

test("the development launcher runtime takes precedence over a workspace setting", (ctx) => {
    const previous = process.env.INTRAVENOUS_DIR;
    ctx.after(() => {
        if (previous === undefined) delete process.env.INTRAVENOUS_DIR;
        else process.env.INTRAVENOUS_DIR = previous;
    });

    const runtime = path.resolve("dev-session", "runtime");
    const configured = path.resolve("build", "src", "intravenous");
    process.env.INTRAVENOUS_DIR = runtime;
    const bundled = path.resolve("dev-bundle", "runtime");
    assert.deepEqual(resolveIntravenousBinary("intravenous", configured, bundled), {
        source: "development bundle",
        path: path.join(bundled, "intravenous"),
    });
    assert.deepEqual(resolveIntravenousBinary("intravenous", configured), {
        source: "INTRAVENOUS_DIR",
        path: path.join(runtime, "intravenous"),
    });

    delete process.env.INTRAVENOUS_DIR;
    assert.deepEqual(resolveIntravenousBinary("intravenous", configured), {
        source: "intravenous.intravenousDir",
        path: path.join(configured, "intravenous"),
    });
    assert.throws(() => resolveIntravenousBinary("intravenous"), /scripts\/run_dev\.sh/);
});

import assert from "node:assert/strict";
import test from "node:test";
import { processStartTime, xWaylandArguments } from "../src/vscodeRelaunch";

test("XWayland restart preserves editor arguments and respects an explicit graphics backend", () => {
    assert.deepEqual(xWaylandArguments(["--user-data-dir=/tmp/editor profile", "--", "--ozone-platform.txt"]),
        ["--user-data-dir=/tmp/editor profile", "--ozone-platform=x11", "--", "--ozone-platform.txt"]);
    assert.deepEqual(xWaylandArguments([]), ["--ozone-platform=x11"]);
    for (const backend of ["x11", "wayland", "auto"]) {
        assert.equal(xWaylandArguments([`--ozone-platform=${backend}`]), null);
        assert.equal(xWaylandArguments(["--ozone-platform", backend]), null);
    }
});

test("a launcher can distinguish a running Linux process from a missing one", { skip: process.platform !== "linux" }, () => {
    const started = processStartTime(process.pid);
    assert.match(started!, /^\d+$/);
    assert.equal(processStartTime(process.pid), started);
    assert.equal(processStartTime(2 ** 31 - 1), null);
});

import assert from "node:assert/strict";
import test from "node:test";
import { fitPopoverAtPoint } from "../src/popoverPosition";

const viewport = { width: 400, height: 240 };
const panel = { width: 160, height: 100 };

test("a point popover stays at the requested position when it fits", () => {
    assert.deepEqual(fitPopoverAtPoint({ x: 200, y: 120 }, panel, viewport), { x: 200, y: 120 });
});

test("a point popover fits inside all four viewport edges", () => {
    assert.deepEqual(fitPopoverAtPoint({ x: 390, y: 230 }, panel, viewport), { x: 236, y: 136 });
    assert.deepEqual(fitPopoverAtPoint({ x: -20, y: -20 }, panel, viewport), { x: 4, y: 4 });
    assert.deepEqual(fitPopoverAtPoint({ x: 12, y: 12 }, panel, { width: 120, height: 60 }), { x: 4, y: 4 });
});

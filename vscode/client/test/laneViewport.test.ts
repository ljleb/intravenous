import test from "node:test";
import assert from "node:assert/strict";

import { requestedLaneViewCount } from "../src/laneViewport";

test("restored empty lane view requests a complete initial page", () => {
    // The browser reports one placeholder row before any lanes have arrived.
    // Treating that as the server window used to strand an unfiltered view on
    // the first public input after an IV package finished loading.
    assert.equal(requestedLaneViewCount(1, 0), 24);
});

test("known lane totals still cover the complete locally ordered result", () => {
    assert.equal(requestedLaneViewCount(1, 37), 37);
    assert.equal(requestedLaneViewCount(64, 37), 64);
});

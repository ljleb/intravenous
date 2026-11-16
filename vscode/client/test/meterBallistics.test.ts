import assert from "node:assert/strict";
import test from "node:test";
import { MeterBallistics } from "../src/meterBallistics";

function close(actual: number, expected: number): void { assert.ok(Math.abs(actual - expected) < 1e-10, `${actual} != ${expected}`); }
function falling(): MeterBallistics {
    const meter = new MeterBallistics();
    meter.reset(1, 1, 0);
    meter.update(0, 0, 0);
    return meter;
}
test("a peak holds briefly then falls with increasing speed", () => {
    const meter = falling();
    assert.equal(meter.advance(119).peak, 1);
    close(meter.advance(220).peak, .98);
    close(meter.advance(320).peak, .92);
    close(meter.advance(420).peak, .82);
});
test("a new higher peak resets the fall momentum", () => {
    const meter = new MeterBallistics(0);
    meter.reset(1, 1, 0);
    meter.update(0, 0, 0);
    close(meter.advance(250).peak, .875);
    assert.equal(meter.update(.95, .5, 250).peak, .95);
    meter.update(0, 0, 260);
    close(meter.advance(360).peak, .931);
});
test("peak and RMS motion depend on elapsed time rather than animation frame count", () => {
    const one = falling().advance(400);
    const many = falling();
    for (let time = 10; time < 400; time += 10) many.advance(time);
    const split = many.advance(400);
    close(one.peak, split.peak);
    close(one.rms, split.rms);
});
test("motion never falls below a measured peak and stops after reaching silence", () => {
    const meter = falling();
    meter.update(.3, .2, 0);
    const settled = meter.advance(5000);
    close(settled.peak, .3);
    close(settled.rms, .2);
    assert.equal(meter.moving, false);
    meter.update(0, 0, 5000);
    assert.deepEqual(meter.advance(10000), { peak: 0, rms: 0 });
    assert.equal(meter.moving, false);
});
test("new activation or unavailable-data reset starts without a previous source's held peak", () => {
    const meter = falling();
    meter.advance(220);
    assert.deepEqual(meter.reset(.1, .05, 230), { peak: .1, rms: .05 });
    assert.equal(meter.moving, false);
    assert.deepEqual(meter.reset(0, 0, 240), { peak: 0, rms: 0 });
});

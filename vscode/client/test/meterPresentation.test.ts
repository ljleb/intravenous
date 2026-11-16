import assert from "node:assert/strict";
import test from "node:test";
import { levelDomain, meterPosition, meterReading, sampleDecibels } from "../src/meterPresentation";
import { numericTicks } from "../webview/timeline/geometry";

test("level grading uses dBFS rather than a linear amplitude fraction", () => {
    assert.equal(sampleDecibels(1), 0);
    assert.ok(Math.abs(sampleDecibels(.5) + 6.0206) < .0001);
    assert.equal(sampleDecibels(0), -Infinity);
    const half = meterReading("level", { available: true, value: { peak: .5, rms: .25, samples: "64", nonfinite: "0" } });
    assert.ok(half.available);
    assert.ok(half.position > .8 && half.position < .83);
    assert.ok(half.rmsPosition < half.position);
    assert.ok(!half.overReference);
    const over = meterReading("level", { available: true, value: { peak: 2, rms: 1, samples: "64", nonfinite: "0" } });
    assert.ok(over.overReference && over.above);
    assert.match(over.text, /\+6\.0 dBFS/);
});

test("value mode uses the same peak and RMS with only a linear scale change", () => {
    const result = (peak: number, rms: number) => ({ available: true, value: { peak, rms, samples: "64", nonfinite: "0" } });
    const linear = meterReading("value", result(.5, .25));
    assert.equal(linear.position, .5);
    assert.equal(linear.rmsPosition, .25);
    assert.equal(linear.displayValue, .5);
    const logarithmic = meterReading("level", result(.5, .25));
    assert.ok(logarithmic.position > linear.position);
    assert.equal(meterReading("value", result(0, 0)).position, 0);
    assert.equal(meterReading("value", result(1, .5)).position, 1);
    const over = meterReading("value", result(3.5, 2));
    assert.equal(over.displayValue, 3.5);
    assert.ok(over.above && over.overReference);
    assert.match(over.text, /3.5 peak/);
    // A last-sample operation is not a peak/RMS measurement in either scale.
    assert.equal(meterReading("value", { available: true, value: { value: -.25 } }).available, false);
});

test("missing and nonfinite measurements stay distinct from measured silence", () => {
    assert.equal(meterReading("level").available, false);
    assert.equal(meterReading("level", { available: false }).available, false);
    assert.equal(meterReading("value", { available: true, value: { value: null } }).available, false);
    assert.equal(meterReading("level", { available: true, value: { peak: 0, rms: 0, samples: "0", nonfinite: "64" } }).available, false);
    assert.match(meterReading("level", { available: true, value: { peak: 0, rms: 0, samples: "0", nonfinite: "64" } }).text, /64 nonfinite/);
    const silent = meterReading("level", { available: true, value: { peak: 0, rms: 0, samples: "64", nonfinite: "0" } });
    assert.equal(silent.available, true);
    assert.equal(silent.position, 0);
    assert.match(silent.text, /−∞/);
});

test("meter and timeline graduations share finite ordered numeric geometry", () => {
    const ticks = numericTicks(levelDomain.min, levelDomain.max, 660);
    assert.ok(ticks.length > 2);
    for (const tick of ticks) assert.equal(tick.x, meterPosition(tick.value, levelDomain.min, levelDomain.max) * 660);
    assert.ok(ticks.every((tick, i) => i === 0 || tick.x > ticks[i - 1].x));
    assert.deepEqual(numericTicks(0, 1, 0), []);
});

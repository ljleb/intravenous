import assert from "node:assert/strict";
import test from "node:test";
import { levelDomain, meterPosition, meterReading, MeterPresentationState, sampleDecibels } from "../src/meterPresentation";
import { ObservationValues, type ObservationDisplayValue, type ObservationUpdate } from "../src/observations";
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

test("output gain scales live and held meter levels before either display scale", () => {
    const meter = new MeterPresentationState();
    meter.update({ available: true, value: { peak: .8, rms: .5, samples: "64" } }, true, 0);
    const recent = { available: true, value: { peak: .25, rms: .125, samples: "64" } };
    meter.update(recent, true, 20);
    const scaled = meter.reading("value", -2);
    assert.equal(scaled.displayValue, .5);
    assert.equal(scaled.rmsPosition, .25);
    assert.equal(scaled.heldOverReference, true);
    assert.equal(meter.reading("level", -2).displayValue, sampleDecibels(.5));
    assert.equal(recent.value.peak, .25);
    assert.equal(meter.reading("value").displayValue, .25);

    meter.update({ available: false, pending: true }, true, 30);
    assert.match(meter.status("value", .5), /previous reading: 0.125 peak/);
    assert.equal(meter.reading("value", .5).heldPosition, .4);
    assert.equal(meter.reading("value", 0).heldPosition, 0);
    assert.equal(meter.reading("value", 0).available, true);
    meter.update({ available: false }, true, 40);
    assert.equal(meter.reading("value", 0).available, false);
});

test("meter and timeline graduations share finite ordered numeric geometry", () => {
    const ticks = numericTicks(levelDomain.min, levelDomain.max, 660);
    assert.ok(ticks.length > 2);
    for (const tick of ticks) assert.equal(tick.x, meterPosition(tick.value, levelDomain.min, levelDomain.max) * 660);
    assert.ok(ticks.every((tick, i) => i === 0 || tick.x > ticks[i - 1].x));
    assert.deepEqual(numericTicks(0, 1, 0), []);
});

test("compatible graph transitions keep latest and held peaks through pending telemetry", () => {
    const meter = new MeterPresentationState(), uninterrupted = new MeterPresentationState();
    const values = new ObservationValues();
    const frame = (generation: string, activationId: string, value: ObservationDisplayValue): ObservationUpdate => ({
        generation, activationId, revision: "1", index: "0", blockSize: 64, dataVersion: "0",
        scopes: [{ scopeId: "viewport", values: [{ id: "meter", ...value }] }],
    });
    const sample = (peak: number, rms: number): ObservationDisplayValue => ({ available: true,
        value: { peak, rms, samples: "64", nonfinite: "0" } });
    for (const state of [meter, uninterrupted]) {
        state.update(sample(1, 1), true, 0);
        state.update(sample(.1, .1), true, 0);
        state.advance(200);
    }
    const before = meter.reading("value");
    assert.equal(before.displayValue, .1);
    assert.ok(before.heldPosition > .9);
    values.acceptGeneration(2);
    meter.update(values.values.get("meter"), true, 200);
    assert.deepEqual(meter.reading("value"), before);
    assert.equal(meter.updating, true);

    assert.equal(values.receive(frame("2", "1", { available: false, pending: true }), "viewport"), true);
    meter.update(values.values.get("meter"), true, 220);
    assert.equal(meter.reading("value").available, true);
    assert.match(meter.status("value"), /Updating.*previous reading/);

    // A delayed frame remains rejected; retaining the picture does not accept
    // an obsolete execution as the configured graph's current measurement.
    assert.equal(values.receive(frame("1", "1", sample(1, 1)), "viewport"), false);
    assert.equal(values.receive(frame("2", "2", sample(.1, .1)), "viewport"), true);
    meter.update(values.values.get("meter"), true, 230);
    uninterrupted.update(sample(.1, .1), true, 230);
    assert.equal(meter.updating, false);
    for (const now of [230, 300, 450]) {
        meter.advance(now);
        uninterrupted.advance(now);
        for (const mode of ["level", "value"] as const) {
            const actual = meter.reading(mode), expected = uninterrupted.reading(mode);
            assert.ok(Math.abs(actual.position - expected.position) < 1e-12);
            assert.ok(Math.abs(actual.heldPosition - expected.heldPosition) < 1e-12);
            assert.ok(Math.abs(actual.rmsPosition - expected.rmsPosition) < 1e-12);
        }
    }
    // Actual source unavailability still invalidates history and motion.
    meter.update({ available: false }, true, 460);
    assert.equal(meter.reading("value").available, false);
    assert.equal(meter.moving, false);
    meter.update(sample(.1, .1), true, 470);
    assert.equal(meter.reading("value").displayValue, .1);
    meter.update(undefined, false, 480);
    assert.equal(meter.reading("value").available, false);
    assert.equal(meter.moving, false);
});

test("the fill follows received peaks immediately while only the held marker has momentum", () => {
    const meter = new MeterPresentationState();
    const sample = (peak: number, rms: number): ObservationDisplayValue => ({ available: true,
        value: { peak, rms, samples: "64", nonfinite: "0" } });
    meter.update(sample(.8, .1), true, 0);
    assert.equal(meter.reading("value").position, .8);
    assert.equal(meter.reading("value").heldPosition, .8);
    meter.update(sample(.2, .05), true, 20);
    assert.equal(meter.reading("value").position, .2);
    assert.equal(meter.reading("value").heldPosition, .8);
    const db = meter.reading("level");
    assert.equal(db.position, meterPosition(sampleDecibels(.2), levelDomain.min, levelDomain.max));
    assert.equal(db.heldPosition, meterPosition(sampleDecibels(.8), levelDomain.min, levelDomain.max));
    meter.advance(220);
    assert.equal(meter.reading("value").position, .2);
    assert.ok(meter.reading("value").heldPosition > .2 && meter.reading("value").heldPosition < .8);
    meter.update(sample(.9, .1), true, 230);
    assert.equal(meter.reading("value").position, .9);
    assert.equal(meter.reading("value").heldPosition, .9);
    assert.match(meter.status("value"), /0\.1 RMS/);
    // A released held peak may remain above reference after the live bar drops.
    meter.update(sample(2, .1), true, 240);
    meter.update(sample(.2, .1), true, 250);
    assert.equal(meter.reading("value").overReference, false);
    assert.equal(meter.reading("value").heldOverReference, true);
    meter.advance(5000);
    assert.equal(meter.moving, false);
});

import assert from "node:assert/strict";
import test from "node:test";
import { knobFraction, knobScrubSpeed } from "../src/knobPresentation";

test("one-sided bounds occupy the corresponding end of a stable knob arc", () => {
    const lower = { min: 10, defaultValue: 30 };
    assert.equal(knobFraction(10, lower), 0);
    assert.equal(knobFraction(0, lower), 0);
    assert.equal(knobFraction(30, lower), .5);
    assert.ok(knobFraction(3000, lower) > .99);
    const upper = { max: 10, defaultValue: -10 };
    assert.equal(knobFraction(10, upper), 1);
    assert.equal(knobFraction(20, upper), 1);
    assert.equal(knobFraction(-10, upper), .5);
    assert.ok(knobFraction(-3000, upper) < .01);
    assert.equal(knobFraction(0, { min: 0, defaultValue: 0 }), 0);
    assert.equal(knobFraction(0, { max: 0, defaultValue: 0 }), 1);
});

test("two-sided and unbounded knob mappings keep their declared centres", () => {
    assert.equal(knobFraction(5, { min: 0, max: 10 }), .5);
    assert.equal(knobFraction(-5, { min: 0, max: 10 }), 0);
    assert.equal(knobFraction(15, { min: 0, max: 10 }), 1);
    assert.equal(knobFraction(220, { defaultValue: 220 }), .5);
    for (const limits of [{ min: 0, defaultValue: 220 }, { max: 100, defaultValue: 0 }, { defaultValue: 0 }]) {
        const fractions = [-10, 0, 1, 5, 220, 10000].map((value) => knobFraction(value, limits));
        assert.ok(fractions.every((value, index) => value >= 0 && value <= 1 && (!index || value >= fractions[index - 1])));
    }
});

test("scrub speed is fixed by the declaration and can be explicitly overridden", () => {
    assert.equal(knobScrubSpeed({ min: -100, max: 100, defaultValue: 0 }), 1);
    assert.equal(knobScrubSpeed({ min: 0, defaultValue: 220 }), 2.2);
    assert.equal(knobScrubSpeed({ defaultValue: 0 }), .01);
    assert.equal(knobScrubSpeed({ min: -100, max: 100, scrubSpeed: .125 }), .125);
    for (const scrubSpeed of [0, -1, Infinity, NaN])
        assert.equal(knobScrubSpeed({ defaultValue: 220, scrubSpeed }), 2.2);
});

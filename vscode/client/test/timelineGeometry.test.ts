import test from "node:test";
import assert from "node:assert/strict";
import { timelineTicks } from "../webview/timeline/geometry";
import { compactWidthPx, editableHeightPx, effectiveDisplayMode, nanoHeightPx } from "../webview/presentation/displayMode";

test("timeline ruler maps time to a shared horizontal position", () => {
    const ticks = timelineTicks({ beginSeconds: 2, endSeconds: 12 }, 900);
    assert.ok(ticks.length > 2);
    for (const tick of ticks) {
        assert.ok(tick.seconds >= 2 && tick.seconds <= 12);
        assert.ok(Math.abs(tick.x - (tick.seconds - 2) * 90) < 1e-9);
        assert.equal(tick.label, `${tick.seconds} s`);
    }
    assert.ok(ticks.every((tick, index) => index === 0 || tick.x > ticks[index - 1].x));
});

test("timeline ruler handles fractional windows and limits label density", () => {
    const ticks = timelineTicks({ beginSeconds: -0.05, endSeconds: 0.05 }, 450);
    assert.ok(ticks.some((tick) => tick.seconds === 0));
    assert.ok(ticks.length <= 7);
    assert.ok(ticks.some((tick) => tick.label.includes("0.02")));
    assert.deepEqual(timelineTicks({ beginSeconds: 1, endSeconds: 1 }, 400), []);
    assert.deepEqual(timelineTicks({ beginSeconds: 0, endSeconds: 10 }, 0), []);
});

test("timeline ruler keeps shared labels stable across horizontal zoom", () => {
    for (const window of [
        { beginSeconds: -2, endSeconds: 4 },
        { beginSeconds: 0, endSeconds: 2 },
        { beginSeconds: 0.9, endSeconds: 1.1 },
    ]) {
        const ticks = timelineTicks(window, 900);
        assert.equal(ticks.find((tick) => Math.abs(tick.seconds - 1) < 1e-12)?.label, "1 s");
    }
    const fractional = timelineTicks({ beginSeconds: -0.5, endSeconds: 0.5 }, 900);
    assert.equal(fractional.find((tick) => Math.abs(tick.seconds - 0.3) < 1e-12)?.label, "0.3 s");
    assert.equal(fractional.find((tick) => Math.abs(tick.seconds + 0.3) < 1e-12)?.label, "-0.3 s");
    assert.equal(fractional.find((tick) => tick.seconds === 0)?.label, "0 s");
});

test("timeline ruler terminates at large absolute times without repeating tick keys", () => {
    const ticks = timelineTicks({ beginSeconds: 1e16, endSeconds: 1e16 + 2 }, 900);
    assert.ok(ticks.length <= 1000);
    assert.equal(new Set(ticks.map((tick) => tick.seconds)).size, ticks.length);
});

test("presentation mode follows allocated height while compact takes precedence", () => {
    assert.equal(effectiveDisplayMode(600, null, null, nanoHeightPx), "nano");
    assert.equal(effectiveDisplayMode(600, null, null, 120), "mini");
    assert.equal(effectiveDisplayMode(600, null, null, editableHeightPx + 30), "editable");
    for (const height of [30, 120, 400]) {
        assert.equal(effectiveDisplayMode(compactWidthPx - 1, null, null, height), "compact");
    }
    assert.equal(effectiveDisplayMode(600, "mini", null, 400), "mini");
});

test("height hysteresis holds a presentation across small threshold changes", () => {
    assert.equal(effectiveDisplayMode(600, null, "nano", nanoHeightPx + 4), "nano");
    assert.equal(effectiveDisplayMode(600, null, "mini", nanoHeightPx + 4), "mini");
    assert.equal(effectiveDisplayMode(600, null, "mini", editableHeightPx + 4), "mini");
    assert.equal(effectiveDisplayMode(600, null, "editable", editableHeightPx + 4), "editable");
});

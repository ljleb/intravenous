import test from "node:test";
import assert from "node:assert/strict";
import { playheadTimeDrag, timelineTimeDrag } from "../src/timelineTimeDrag";
import { minimumBeatMarkSpacing, selectVisibleBeatMarks } from "../src/beatGrid";
import { gridGuideStrengths } from "../src/gridGuideStrength";
import { seekBeatWindow, snapTimelineSample, timelineBeatObservationWindow, timelineSeamX, timelineTicks } from "../src/timelineGeometry";
import { firstSignedSample, timelineSampleCount } from "../src/timelineSamples";
import { compactWidthPx, editableHeightPx, effectiveDisplayMode, nanoHeightPx } from "../webview/presentation/displayMode";

test("the ruler keeps coarser coincident beats and snapping only uses the displayed marks", () => {
    const displayed = selectVisibleBeatMarks([
        { index: 160n, x: 16, depth: 3 },
        { index: 110n, x: 11, depth: 2 },
        { index: 240n, x: 24, depth: 1 },
        { index: 160n, x: 16, depth: 0 },
        { index: 60n, x: 6, depth: 1 },
        { index: 0n, x: 0, depth: 2 },
    ], 40);
    assert.deepEqual(displayed.map(({ index, depth }) => [index, depth]), [[60n, 1], [160n, 0], [240n, 1]]);
    const targets = displayed.map(({ index }) => index);
    assert.equal(snapTimelineSample(112n, targets, 5), 112n);
    assert.equal(snapTimelineSample(165n, targets, 5), 160n);
    assert.equal(snapTimelineSample(166n, targets, 5), 166n);
});

test("beat subdivision visibility follows current spacing in both zoom directions", () => {
    const marks = [
        { index: 0n, x: 0, depth: 0 },
        { index: 1n, x: 1, depth: 2 },
        { index: 2n, x: 2, depth: 1 },
        { index: 3n, x: 3.05, depth: 2 },
    ];
    const visible = (scale: number) => selectVisibleBeatMarks(marks.map((mark) => ({ ...mark, x: mark.x * scale })), 80)
        .map(({ index }) => index);
    assert.deepEqual(visible(7.9), [0n, 2n, 3n]);
    assert.deepEqual(visible(minimumBeatMarkSpacing), [0n, 1n, 2n, 3n]);
    assert.deepEqual(visible(7.9), [0n, 2n, 3n]);
    assert.deepEqual(selectVisibleBeatMarks([
        { index: -1n, x: -3, depth: 0 }, { index: 0n, x: 2, depth: 1 }, { index: 1n, x: 14, depth: 2 },
    ], 20).map(({ index }) => index), [1n]);
});

test("dense bars dim smoothly even when no subdivision is visible", () => {
    const bars = [{ x: 0, depth: 0 }, { x: 1, depth: 0 }, { x: 4, depth: 0 }];
    const at = (scale: number) => gridGuideStrengths(bars, ({ x }) => x * scale).get(0)!;
    assert.ok(at(8) < .07);
    assert.ok(at(8) < at(16) && at(16) < at(48));
    assert.ok(Math.abs(at(8 + .001) - at(8 - .001)) < .001);
    assert.equal(at(8), gridGuideStrengths(bars.slice(0, 2), ({ x }) => x * 8).get(0));
});

test("guide strengths use the closest pair at each visible depth and adapt to missing levels", () => {
    const marks = [
        { x: 96, depth: 3 }, { x: 0, depth: 1 }, { x: 8, depth: 3 },
        { x: 112, depth: 1 }, { x: 104, depth: 2 }, { x: 56, depth: 2 },
    ];
    const strengths = gridGuideStrengths(marks, ({ x }) => x);
    const falloff = (gap: number) => 1 / (1 + (24 / gap) ** 2);
    const brightness = .6 + .4 * falloff(8);
    assert.equal(strengths.get(1), brightness * falloff(112));
    assert.equal(strengths.get(2), brightness * falloff(8) * .82);
    assert.equal(strengths.get(3), brightness * falloff(8) * .82 ** 2);
    const finest = marks.filter(({ depth }) => depth === 3);
    assert.ok(gridGuideStrengths(finest, ({ x }) => x).get(3)! > strengths.get(3)!);
});

test("beat observations share a padded query across nearby pans and zooms", () => {
    const base = timelineBeatObservationWindow(-2001n, 3000n, 1200, true)!;
    assert.deepEqual(timelineBeatObservationWindow(-1200n, 3300n, 1200, true), base);
    assert.equal(base.points, 1024);
    assert.ok(BigInt(base.begin) < -2001n);
    assert.ok(BigInt(base.begin) + BigInt(base.length) > -1200n + 3300n);
    assert.notDeepEqual(timelineBeatObservationWindow(50n, 3300n, 1200, true), base);
    assert.notDeepEqual(timelineBeatObservationWindow(-1200n, 4300n, 1200, true), base);
    assert.deepEqual(timelineBeatObservationWindow(-2001n, 3000n, 1200, false),
        { begin: "-2001", length: "3000", points: 1024, maxDepth: 255 });
    const nearSeam = (1n << 63n) + 20n;
    const wrapped = timelineBeatObservationWindow(nearSeam, 100n, 800, true)!;
    const offset = BigInt.asUintN(64, nearSeam - BigInt(wrapped.begin));
    assert.ok(offset > 0n && offset + 100n < BigInt(wrapped.length));
});

test("playhead seek windows lead past both viewport edges and overlap as they move", () => {
    const visible = { first: 100n, length: 100n };
    assert.equal(seekBeatWindow(visible, 150n, true), undefined);
    assert.equal(seekBeatWindow(visible, 174n, true), undefined);
    assert.deepEqual(seekBeatWindow(visible, 175n, true), { first: 150n, length: 200n });
    assert.deepEqual(seekBeatWindow(visible, 275n, true), { first: 250n, length: 200n });
    assert.deepEqual(seekBeatWindow(visible, 124n, false), { first: -50n, length: 200n });
    const last = (1n << 63n) - 1n;
    assert.deepEqual(seekBeatWindow({ first: last - 75n, length: 100n }, -last, true),
        { first: last - 25n, length: 200n });
});

test("visible beats become detents when the grid loads during the first playhead drag", () => {
    let visible: bigint[] = [];
    const drag = playheadTimeDrag(0n, { spanSamples: 1, width: 1 }, () => visible);
    assert.equal(drag(124).sampleIndex, 124n);
    visible = [110n, 140n];
    assert.equal(drag(124).sampleIndex, 124n);
    assert.equal(drag(140).snappedIndex, 140n);
    assert.equal(drag(164).snappedIndex, 140n);
    assert.equal(drag(165).sampleIndex, 141n);
});

test("playhead detents arrive without a jump and retain their remaining travel when a window changes", () => {
    let loaded: bigint[] | undefined;
    const drag = playheadTimeDrag(0n, { spanSamples: 1, width: 1 }, () => [], () => loaded);
    assert.equal(drag(124).sampleIndex, 124n);
    loaded = [110n, 140n];
    assert.equal(drag(124).sampleIndex, 124n);
    assert.equal(drag(140).snappedIndex, 140n);
    assert.equal(drag(146).plateau, -6);
    loaded = [175n];
    assert.equal(drag(147).plateau, -5);
    assert.equal(drag(164).snappedIndex, 140n);
    assert.equal(drag(165).sampleIndex, 141n);
    assert.equal(drag(199).snappedIndex, 175n);

    loaded = [160n];
    assert.equal(drag(173).snappedIndex, 160n);
});

test("replacing seek targets preserves sub-sample travel", () => {
    let loaded: bigint[] | undefined;
    const drag = playheadTimeDrag(0n, { spanSamples: .4, width: 1 }, () => [], () => loaded);
    assert.equal(drag(1).sampleIndex, 0n);
    loaded = [100n];
    assert.equal(drag(2).sampleIndex, 1n);
    loaded = [110n];
    assert.equal(drag(3).sampleIndex, 1n);
    loaded = [120n];
    assert.equal(drag(4).sampleIndex, 2n);
});

test("drag detents add pointer travel and keep the adjacent samples reachable in either direction", () => {
    const anchor = (1n << 53n) + 1n;
    const drag = timelineTimeDrag(anchor, { spanSamples: 1, width: 1 }, [anchor + 20n, anchor + 20n, anchor - 20n]);
    for (const direction of [-1, 1]) {
        const sign = BigInt(direction);
        assert.equal(drag(19 * direction).sampleIndex, anchor + sign * 19n);
        for (const travel of [20, 32, 44])
            assert.equal(drag(travel * direction).snappedIndex, anchor + sign * 20n);
        assert.equal(drag(45 * direction).sampleIndex, anchor + sign * 21n);
        assert.equal(drag(45 * direction).snappedIndex, undefined);
    }
});

test("dragging from a snap target starts inside its detent", () => {
    const drag = timelineTimeDrag(100n, { spanSamples: 1, width: 1 }, [100n, 120n]);
    assert.equal(drag(12).sampleIndex, 100n);
    assert.equal(drag(13).sampleIndex, 101n);
    assert.equal(drag(31).sampleIndex, 119n);
    assert.equal(drag(56).sampleIndex, 120n);
    assert.equal(drag(57).sampleIndex, 121n);
});

function secondsView(begin: number, end: number) {
    const sampleRate = 48000;
    return { beginSample: String(BigInt(Math.round(begin * sampleRate))), beginFraction: 0,
        spanSamples: (end - begin) * sampleRate, sampleRate };
}

test("timeline ruler maps time to a shared horizontal position", () => {
    const ticks = timelineTicks(secondsView(2, 12), 900);
    assert.ok(ticks.length > 2);
    for (const tick of ticks) {
        assert.ok(tick.seconds >= 2 && tick.seconds <= 12);
        assert.ok(Math.abs(tick.x - (tick.seconds - 2) * 90) < 1e-9);
        assert.equal(tick.label, `${tick.seconds} s`);
    }
    assert.ok(ticks.every((tick, index) => index === 0 || tick.x > ticks[index - 1].x));
});

test("timeline ruler handles fractional windows and limits label density", () => {
    const ticks = timelineTicks(secondsView(-0.05, 0.05), 450);
    assert.ok(ticks.some((tick) => tick.seconds === 0));
    assert.ok(ticks.length <= 7);
    assert.ok(ticks.some((tick) => tick.label.includes("0.02")));
    assert.deepEqual(timelineTicks(secondsView(1, 1), 400), []);
    assert.deepEqual(timelineTicks(secondsView(0, 10), 0), []);
});

test("timeline ruler keeps shared labels stable across horizontal zoom", () => {
    const positions: number[] = [];
    for (const window of [
        secondsView(-2, 4),
        secondsView(0, 2.5),
        secondsView(0.9, 1.2),
    ]) {
        const ticks = timelineTicks(window, 900);
        const tick = ticks.find(({ seconds }) => Math.abs(seconds - 1) < 1e-12);
        assert.ok(tick);
        assert.equal(tick.label, "1 s");
        assert.equal(tick.key, "1 s");
        positions.push(tick.x);
    }
    assert.equal(new Set(positions).size, 3);
    const fractional = timelineTicks(secondsView(-0.5, 0.5), 900);
    assert.equal(fractional.find((tick) => Math.abs(tick.seconds - 0.3) < 1e-12)?.label, "0.3 s");
    assert.equal(fractional.find((tick) => Math.abs(tick.seconds + 0.3) < 1e-12)?.label, "-0.3 s");
    assert.equal(fractional.find((tick) => tick.seconds === 0)?.label, "0 s");
});

test("timeline ruler terminates at large absolute times without repeating tick keys", () => {
    const ticks = timelineTicks({ beginSample: String(-firstSignedSample - 8000n), beginFraction: 0, spanSamples: 16000, sampleRate: 48000 }, 900);
    assert.ok(ticks.length <= 1000);
    assert.equal(new Set(ticks.map((tick) => tick.key)).size, ticks.length);
});

test("timeline ticks follow the signed sample seam without losing detail at large indices", () => {
    const time = { beginSample: String(-firstSignedSample - 5n), beginFraction: 0, spanSamples: 10, sampleRate: 1 };
    const ticks = timelineTicks(time, 900, true);
    const atSeam = ticks.find(({ x }) => x === 450);
    assert.equal(atSeam?.sampleLabel, `${firstSignedSample} smp`);
    assert.ok(ticks.some(({ sampleLabel }) => sampleLabel === `${-firstSignedSample - 1n} smp`));
    assert.ok(ticks.some(({ sampleLabel }) => sampleLabel === `${firstSignedSample + 1n} smp`));
    const full = { ...time, beginSample: "0", spanSamples: Number(timelineSampleCount) };
    assert.equal(timelineSeamX(full, 900), 450);
    const wideTicks = timelineTicks(full, 900);
    assert.ok(wideTicks.some(({ seconds, x }) => seconds > 0 && x < 450));
    assert.ok(wideTicks.some(({ seconds, x }) => seconds < 0 && x > 450));
    assert.ok(wideTicks.every(({ x }, index) => index === 0 || x > wideTicks[index - 1].x));
});

test("a seek drag of one viewport width returns to the same sample at maximum zoom out", () => {
    const width = 973.5;
    const anchor = firstSignedSample + 11n;
    const drag = playheadTimeDrag(anchor, { spanSamples: Number(timelineSampleCount), width }, () => []);
    assert.equal(drag(width / 2).sampleIndex, 11n);
    assert.equal(drag(width).sampleIndex, anchor);
    assert.equal(drag(-width).sampleIndex, anchor);
});

test("presentation mode follows allocated height while compact takes precedence", () => {
    assert.equal(effectiveDisplayMode(600, null, null, nanoHeightPx), "nano");
    assert.equal(effectiveDisplayMode(600, null, null, 32), "mini");
    assert.equal(effectiveDisplayMode(600, null, null, editableHeightPx + 30), "editable");
    for (const height of [30, 120, 400]) {
        assert.equal(effectiveDisplayMode(compactWidthPx - 1, null, null, height), "compact");
    }
    assert.equal(effectiveDisplayMode(600, "mini", null, 400), "mini");
});

test("vertical zoom selects the same mode in both directions", () => {
    const heights = [12, nanoHeightPx, 32, editableHeightPx - 1, editableHeightPx, 64];
    const expected = ["nano", "nano", "mini", "mini", "editable", "editable"] as const;
    for (const previous of ["nano", "mini", "editable"] as const)
        heights.forEach((height, i) => assert.equal(effectiveDisplayMode(600, null, previous, height), expected[i]));
    let previous: "nano" | "mini" | "editable" | "compact" = "mini";
    for (const i of [2, 4, 5, 4, 2, 0, 2, 4, 5, 4, 2]) {
        previous = effectiveDisplayMode(600, null, previous, heights[i]);
        assert.equal(previous, expected[i]);
    }
});

import test from "node:test";
import assert from "node:assert/strict";
import { createSSRApp, defineComponent, h } from "vue";
import { renderToString } from "vue/server-renderer";
import { isPendingPlaybackSeek, PlaybackDisplay, playbackSampleIndexFromSeconds,
    playbackSeekPosition, type PlaybackSnapshot } from "../src/playback";
import { WorkspaceRpc } from "../src/workspaceRpc";
import { PlaybackProfile } from "../src/playbackProfile";
import type { JsonRpcSocketClient } from "../src/rpcClient";
import { providePlayback, usePlayback } from "../webview/timeline/playbackContext";

const state = (changes: Partial<PlaybackSnapshot> = {}): PlaybackSnapshot => ({
    position: "48000", startPosition: "48000", executionIndex: "48000",
    sampleRate: 48000, running: true, foregroundRunning: true, appliedCommand: "1", segment: "1", ...changes,
});
test("the displayed playhead stays at the drop while the final transport request is pending", async (t) => {
    const cancelFrame = Object.getOwnPropertyDescriptor(globalThis, "cancelAnimationFrame");
    Object.defineProperty(globalThis, "cancelAnimationFrame", { configurable: true, value: () => {} });
    t.after(() => {
        if (cancelFrame) Object.defineProperty(globalThis, "cancelAnimationFrame", cancelFrame);
        else Reflect.deleteProperty(globalThis, "cancelAnimationFrame");
    });
    let playback!: ReturnType<typeof providePlayback>;
    let view!: ReturnType<typeof usePlayback>;
    const child = defineComponent({ setup() { view = usePlayback(); return () => null; } });
    await renderToString(createSSRApp({ setup() { playback = providePlayback(); return () => h(child); } }));
    const update = (snapshot: PlaybackSnapshot, requestedPosition: string | null, releasedDragPosition: string | null,
        dragReleaseId?: string) => playback.receive({ type: "playbackUpdate", snapshot, requestedPosition, releasedDragPosition, dragReleaseId });

    const paused = state({ running: false, executionIndex: undefined });
    update(paused, null, null);
    view.dragPosition.value = "49000";
    const releaseId = view.releaseDrag("49000");
    update(paused, "48050", null, "earlier-drag");
    assert.equal(view.backgroundPlayhead.value, 49000n);
    update(paused, "49000", "49000", releaseId);
    assert.equal(view.backgroundPlayhead.value, 49000n);
    update({ ...paused, position: "49000", startPosition: "49000" }, null, null, releaseId);
    assert.equal(view.backgroundPlayhead.value, 49000n);
    assert.equal(view.dragPreview.value, null);

    update(state({ running: false, foregroundRunning: false }), null, null);
    view.dragPosition.value = "50000";
    assert.equal(view.backgroundPlayhead.value, 50000n);
    assert.equal(view.foregroundPlayhead.value, 50000n);
});
test("seek retains exact sample indices and quantizes seconds at the project rate", () => {
    assert.equal(playbackSeekPosition("-9223372036854775808", 48000), "-9223372036854775808");
    assert.equal(playbackSeekPosition("9223372036854775807", 48000), "9223372036854775807");
    assert.equal(playbackSeekPosition("1.5s", 48000), "72000");
    assert.equal(playbackSeekPosition("-0.5 s", 96000), "-48000");
    assert.equal(playbackSeekPosition("0.00001s", 48000), "0");
    assert.equal(playbackSampleIndexFromSeconds(1e-7, 48000), "0");
    for (const value of ["", "1.5", "9223372036854775808", "-9223372036854775809", "Infinitys", "1e100s"])
        assert.throws(() => playbackSeekPosition(value, 48000));
});
test("position animation stops on pause, starts a new seek segment and bounds stale extrapolation", () => {
    const display = new PlaybackDisplay();
    assert.equal(display.backgroundPlayhead(100), null);
    display.update(state(), 100);
    assert.equal(display.backgroundPlayhead(150), 50400n);
    assert.equal(display.backgroundPlayhead(10000), 60000n);
    display.update(state({ position: "48000", running: false, segment: "2" }), 200);
    assert.equal(display.backgroundPlayhead(10000), 48000n);
    display.update(state({ position: "-24000", segment: "3" }), 300);
    assert.equal(display.backgroundPlayhead(350), -21600n);
    display.update(null, 400);
    assert.equal(display.backgroundPlayhead(500), null);
});
test("animation keeps exact integer positions and wraps on the cyclic timeline", () => {
    const display = new PlaybackDisplay();
    display.update(state({ position: "9223372036854775807", sampleRate: 1000 }), 0);
    assert.equal(display.backgroundPlayhead(2), -9223372036854775807n);
});
test("repeated running snapshots hold the applied cursor during an underrun", () => {
    const display = new PlaybackDisplay();
    display.update(state(), 0);
    display.update(state(), 50);
    assert.equal(display.backgroundPlayhead(75), 48000n);
    display.update(state({ position: "48064" }), 100);
    assert.equal(display.backgroundPlayhead(125), 49264n);
});
test("the foreground playhead follows Tick progress while the background playhead is held", () => {
    const display = new PlaybackDisplay();
    display.update(state(), 0);
    assert.equal(display.foregroundPlayhead(25), display.backgroundPlayhead(25));

    const paused = state({ running: false, executionIndex: "50400", segment: "2" });
    display.update(paused, 50);
    assert.equal(display.backgroundPlayhead(75), 48000n);
    assert.equal(display.foregroundPlayhead(75), 51600n);
    assert.equal(display.isAnimating(75), true);
    assert.equal(display.foregroundPlayhead(1000), 62400n);
    assert.equal(display.isAnimating(1000), false);

    display.update(paused, 100);
    assert.equal(display.foregroundPlayhead(125), 50400n);
    assert.equal(display.isAnimating(125), false);
    display.update(state({ executionIndex: undefined }), 150);
    assert.equal(display.foregroundPlayhead(175), null);
});
test("requested cursor waits for its exact accepted command or a superseding one", () => {
    const seek = { position: "-300", acceptedCommand: "9007199254740993" };
    assert.equal(isPendingPlaybackSeek({ position: seek.position }, state()), true);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: "9007199254740992" })), true);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: seek.acceptedCommand })), false);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: "9007199254740994" })), false);
});
test("parking the foreground playhead stops extrapolation immediately", () => {
    const display = new PlaybackDisplay();
    display.update(state({ executionIndex: "50400", position: "50400" }), 0);
    display.update(state({ running: false, foregroundRunning: false, segment: "2" }), 30);
    assert.equal(display.foregroundPlayhead(55), 48000n);
    assert.equal(display.backgroundPlayhead(55), 48000n);
    assert.equal(display.isAnimating(55), false);
    display.update(state({ running: false, executionIndex: "48128", segment: "3" }), 65);
    assert.equal(display.foregroundPlayhead(90), 49328n);
    assert.equal(display.backgroundPlayhead(90), 48000n);
});
test("playback RPC does not attach graph revisions or touch project history", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method: string, params: unknown) => { calls.push({ method, params }); return state(); } };
    const rpc = new WorkspaceRpc(client as unknown as JsonRpcSocketClient, () => 99);
    await rpc.getPlayback();
    await rpc.getPlayback(true);
    await rpc.getPlayback(false, { id: "take", durationMs: 10000 });
    await rpc.getPlayback(false, { id: "take" });
    await rpc.togglePlayback();
    await rpc.togglePlayback(true);
    await rpc.seekPlayback("-9223372036854775808");
    assert.deepEqual(calls, [
        { method: "playback.get", params: {} }, { method: "playback.get", params: { diagnostics: true } },
        { method: "playback.get", params: { outputProfile: { id: "take", durationMs: 10000 } } },
        { method: "playback.get", params: { outputProfile: { id: "take" } } },
        { method: "playback.toggle", params: { foreground: false } },
        { method: "playback.toggle", params: { foreground: true } },
        { method: "playback.seek", params: { position: "-9223372036854775808" } },
    ]);
});

test("the playback profile correlates probe changes, execution cutovers, and marked clicks", () => {
    const profile = new PlaybackProfile(1000);
    const diagnostics = { configuredGeneration: "7", executionGeneration: "42",
        backgroundUnderrunBlocks: "9007199254740993" };
    profile.recordPlayback(state({ diagnostics }), 1000, 1002);
    const compiling = { phase: "compiling", message: "", preparedGeneration: null, activeGeneration: 42 };
    profile.recordGraph(compiling, 1060);
    profile.recordGraph(compiling, 1070);
    profile.recordObservation({ revision: "4", activationId: "8" }, 1110);
    profile.recordPlayback(state({ diagnostics: { ...diagnostics, executionGeneration: "43",
        backgroundUnderrunBlocks: "9007199254740995" }, executionIndex: "48096" }), 1100, 1120);
    profile.markClick(1150);
    const report = profile.report(11000);
    assert.match(report, /Observed execution changes · 1, including 1 within the same graph configuration/);
    assert.match(report, /Background underruns · 2 blocks silenced/);
    assert.match(report, /\+0\.110 s  Live meter probes · requirement revision 4, activation 8/);
    assert.match(report, /\+0\.120 s  Active execution 42 → 43, configuration 7, foreground sample 48096/);
    assert.match(report, /Audible clicks marked · 1 · \+0\.150 s/);
    assert.equal(report.match(/Graph compiling/g)?.length, 1);
});

test("a playback profile with missing diagnostics reports unavailable measurements", () => {
    const profile = new PlaybackProfile(0);
    profile.recordPlayback(state(), 0, 20);
    profile.recordPlayback(null, 50, 75);
    const report = profile.report(10000);
    assert.match(report, /0 with diagnostics, 2 unavailable/);
    assert.match(report, /server returned no playback diagnostics/);
    assert.doesNotMatch(report, /Background underruns · 0/);
});

test("the device capture compares each channel at the graph switch with nearby sample steps", () => {
    const profile = new PlaybackProfile(1000);
    for (let i = 0; i < 125; ++i) profile.recordObservation({ revision: "4", activationId: String(i) }, 1000 + i);
    profile.markClick(1500);
    profile.stop(2000);
    profile.markClick(2050);
    const report = profile.report(2200, {
        status: "ready", buffers: 3, overBudgetBuffers: 1, omittedCutovers: 0,
        maxRenderMs: 3, maxBudgetFraction: 1.5, cutovers: [{
            previousGeneration: "42", executionGeneration: "42", previousPublication: "12",
            codePublication: "13", foregroundSample: "-9007199254740995",
            previousRendered: true, rendered: true, transportChanged: false,
            previousTransitionTick: false, transitionTick: false,
            elapsedMs: 420, renderMs: 3, budgetMs: 2,
            before: [0, null, 0.25, null], after: [0.75, 0.5, 0.875, 0.25],
        }],
    });
    assert.match(report, /over 1\.00 s/);
    assert.match(report, /Audible clicks marked · 1 · \+0\.500 s/);
    assert.match(report, /1 exceeded their audio duration.*150\.0%/);
    assert.match(report, /\+0\.420 s · foreground sample -9007199254740995 · execution 42 → 42, code publication 12 → 13/);
    assert.match(report, /step L\/R 0\.50000 \/ \? · nearby peak step L\/R 0\.25000 \/ \? before, 0\.12500 \/ 0\.25000 after/);
    assert.match(report, /5 later timeline entries omitted/);
});

import test from "node:test";
import assert from "node:assert/strict";
import { isPendingPlaybackSeek, PlaybackDisplay, playbackSampleIndexFromSeconds,
    playbackSeekPosition, type PlaybackSnapshot } from "../src/playback";
import { WorkspaceRpc } from "../src/workspaceRpc";
import type { JsonRpcSocketClient } from "../src/rpcClient";

const state = (changes: Partial<PlaybackSnapshot> = {}): PlaybackSnapshot => ({
    position: "48000", sampleRate: 48000, running: true, appliedCommand: "1", segment: "1", ...changes,
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
    assert.equal(display.position(100), null);
    display.update(state(), 100);
    assert.equal(display.position(150), 50400n);
    assert.equal(display.position(10000), 60000n);
    display.update(state({ position: "48000", running: false, segment: "2" }), 200);
    assert.equal(display.position(10000), 48000n);
    display.update(state({ position: "-24000", segment: "3" }), 300);
    assert.equal(display.position(350), -21600n);
    display.update(null, 400);
    assert.equal(display.position(500), null);
});
test("animation keeps exact integer positions and wraps on the cyclic timeline", () => {
    const display = new PlaybackDisplay();
    display.update(state({ position: "9223372036854775807", sampleRate: 1000 }), 0);
    assert.equal(display.position(2), -9223372036854775807n);
});
test("repeated running snapshots hold the applied cursor during an underrun", () => {
    const display = new PlaybackDisplay();
    display.update(state(), 0);
    display.update(state(), 50);
    assert.equal(display.position(75), 48000n);
    display.update(state({ position: "48064" }), 100);
    assert.equal(display.position(125), 49264n);
});
test("requested cursor waits for its exact accepted command or a superseding one", () => {
    const seek = { position: "-300", acceptedCommand: "9007199254740993" };
    assert.equal(isPendingPlaybackSeek({ position: seek.position }, state()), true);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: "9007199254740992" })), true);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: seek.acceptedCommand })), false);
    assert.equal(isPendingPlaybackSeek(seek, state({ appliedCommand: "9007199254740994" })), false);
});
test("playback RPC does not attach graph revisions or touch project history", async () => {
    const calls: unknown[] = [];
    const client = { request: async (method: string, params: unknown) => { calls.push({ method, params }); return state(); } };
    const rpc = new WorkspaceRpc(client as unknown as JsonRpcSocketClient, () => 99);
    await rpc.getPlayback();
    await rpc.togglePlayback();
    await rpc.seekPlayback("-9223372036854775808");
    assert.deepEqual(calls, [
        { method: "playback.get", params: {} }, { method: "playback.toggle", params: {} },
        { method: "playback.seek", params: { position: "-9223372036854775808" } },
    ]);
});

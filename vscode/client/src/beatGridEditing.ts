import { beatBpm, sameBeatGrid, readBeatGrid, type BeatGrid } from "./beatGrid";
import { SetBeatGrid } from "./generated/beatsEvents";
import { addressKey, type GraphScope, type NodeAddress } from "./graphScope";
import type { NodeEventGesture } from "./nodeEventMessaging";
import { NodeEventUpdates, type NodeEventUpdateTimers } from "./nodeEventUpdates";

type Edit = { desired: BeatGrid; sent: BeatGrid; gesture: NodeEventGesture; updates: NodeEventUpdates };
type Track = {
    grid: BeatGrid;
    draft?: { grid: BeatGrid; revision?: number };
    active?: Edit;
    closing: Promise<void>;
    epoch: number;
    error?: string;
};
export class BeatGridEditing {
    private readonly tracks = new Map<string, Track>();
    private readRevision = 0;
    constructor(private readonly transport: { gesture(target: NodeAddress): NodeEventGesture }, private readonly changed: () => void,
        private readonly timers?: NodeEventUpdateTimers) {}

    reconcile(scope: GraphScope | null): void {
        const revision = scope?.sessionRevision ?? this.readRevision;
        if (revision < this.readRevision) return;
        this.readRevision = revision;
        const seen = new Set<string>();
        for (const { address, construction } of scope?.nodes ?? []) {
            if (construction?.definitionId !== "builtin.beats") continue;
            const grid = readBeatGrid(construction.arguments);
            if (!grid) continue;
            const key = addressKey(address);
            seen.add(key);
            const track = this.tracks.get(key);
            if (!track) this.tracks.set(key, { grid, closing: Promise.resolve(), epoch: 0 });
            else {
                if (!sameBeatGrid(track.grid, grid)) track.grid = grid;
                if (track.draft?.revision !== undefined && revision >= track.draft.revision) track.draft = undefined;
            }
        }
        for (const [key, track] of this.tracks) {
            if (seen.has(key)) continue;
            this.abandonTrack(track);
            this.tracks.delete(key);
        }
        this.changed();
    }
    view(target: NodeAddress | undefined): BeatGrid | undefined {
        const track = target && this.tracks.get(addressKey(target));
        return track?.draft?.grid ?? track?.grid;
    }
    error(target: NodeAddress | undefined): string {
        return (target && this.tracks.get(addressKey(target))?.error) ?? "";
    }
    update(target: NodeAddress, grid: BeatGrid, continuous = false): void {
        const track = this.tracks.get(addressKey(target));
        if (!track) return;
        const before = track.draft?.grid ?? track.grid;
        if (sameBeatGrid(before, grid)) return;
        track.error = undefined;
        track.draft = { grid };
        if (track.active) track.active.desired = grid;
        else track.active = this.begin(target, track, before, grid);
        if (continuous) track.active.updates.schedule();
        else void this.finish(target);
        this.changed();
    }
    private begin(target: NodeAddress, track: Track, sent: BeatGrid, next: BeatGrid): Edit {
        const before = track.closing, epoch = track.epoch;
        const edit: Edit = { desired: next, sent, gesture: this.transport.gesture(target),
            updates: new NodeEventUpdates(async () => {
                await before;
                const value = edit.desired;
                if (epoch !== track.epoch || sameBeatGrid(edit.sent, value)) return false;
                await edit.gesture.send(SetBeatGrid({ bpm: beatBpm(value), beats_per_bar: value.beatsPerBar,
                    subdivisions: [...value.subdivisions] }));
                edit.sent = value;
                return true;
            }, async (error) => {
                const closing = edit.gesture.finish().catch(() => {});
                if (track.active === edit) { track.active = undefined; track.closing = closing; }
                if (epoch === track.epoch) this.fail(track, error);
                await closing;
            }, this.timers) };
        return edit;
    }
    finish(target: NodeAddress): Promise<void> {
        const track = this.tracks.get(addressKey(target));
        if (!track?.active) return track?.closing ?? Promise.resolve();
        const edit = track.active, epoch = track.epoch;
        track.active = undefined;
        track.closing = edit.updates.flush().then(async () => {
            try { await edit.gesture.finish(); }
            catch (error) { if (epoch === track.epoch) this.fail(track, error); return; }
            if (track.draft?.grid === edit.desired) {
                const revision = edit.gesture.acceptedRevision?.();
                if (revision !== undefined && revision > this.readRevision) track.draft.revision = revision;
                else track.draft = undefined;
                this.changed();
            }
        }, () => {});
        return track.closing;
    }
    private fail(track: Track, error: unknown): void {
        ++track.epoch;
        track.draft = undefined;
        track.error = error instanceof Error ? error.message : String(error);
        this.changed();
    }
    private abandonTrack(track: Track): void {
        ++track.epoch;
        track.draft = undefined;
        const edit = track.active;
        if (!edit) return;
        track.active = undefined;
        track.closing = edit.updates.stop().catch(() => {}).then(() => edit.gesture.finish()).catch(() => {});
    }
    abandon(): void {
        for (const track of this.tracks.values()) this.abandonTrack(track);
        this.changed();
    }
    disconnected(): void {
        this.abandon();
        this.tracks.clear();
        this.readRevision = 0;
        this.changed();
    }
}

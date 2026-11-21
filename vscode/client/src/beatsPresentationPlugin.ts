import { beatBpm, beatGridTicks, defaultBeatBpm, maximumBeatBpm, maximumBeatsPerBar, maximumBeatSubdivisionCount,
    minimumBeatBpm, readBeatGrid, sameBeatGrid, type BeatGrid } from "./beatGrid";
import { SetBeatGrid } from "./generated/beatsEvents";
import { serializeNodeEvent } from "./nodeEvents";
import { gridGuideStrengths } from "./gridGuideStrength";
import type { Present, PresentationCommand, PresentationControl, PresentationFrame, PresentationInput, PresentationManifest, PresentationMark } from "./presentationPlugin";

export const beatsPresentationManifest: PresentationManifest = {
    version: 1, id: "builtin.beats", title: "Beats", definitionIds: ["builtin.beats"], timeline: true,
    icon: "M2 3v10m4-7v7m4-4v4m4-7v7",
};

export function beatSettings(grid: BeatGrid, sampleRate?: number) {
    const divisions = grid.subdivisions.reduce((product, factor) => product * factor, 1);
    const limit = maximumBeatSubdivisionCount(grid, sampleRate);
    const knob = (id: string, label: string, value: number, min: number, max: number, defaultValue: number): PresentationControl =>
        ({ type: "knob", id, label, value, min, max, defaultValue, integer: true });
    return {
        addAllowed: divisions * 2 <= limit,
        controls: [
            knob("bpm", "BPM", beatBpm(grid), minimumBeatBpm, maximumBeatBpm(sampleRate, divisions), defaultBeatBpm),
            knob("beatsPerBar", "Beats per bar", grid.beatsPerBar, 1, maximumBeatsPerBar, 4),
            ...grid.subdivisions.map((factor, index) => knob(`subdivision:${index}`, `Subdivision ${index + 1}`, factor,
                2, Math.floor(limit / (divisions / factor)), 2)),
        ],
    };
}

export function editBeatSetting(grid: BeatGrid, input: Extract<PresentationInput, { type: "control" }>, sampleRate?: number): BeatGrid | undefined {
    const settings = beatSettings(grid, sampleRate);
    if (input.id === "add" && settings.addAllowed) return readBeatGrid([beatBpm(grid), grid.beatsPerBar, [...grid.subdivisions, 2]]) ?? undefined;
    if (input.id.startsWith("remove:")) {
        const index = Number(input.id.slice("remove:".length));
        if (!Number.isInteger(index) || index < 0 || index >= grid.subdivisions.length) return;
        return readBeatGrid([beatBpm(grid), grid.beatsPerBar, grid.subdivisions.filter((_, at) => at !== index)]) ?? undefined;
    }
    const knob = settings.controls.find((control) => control.id === input.id);
    if (knob?.type !== "knob" || knob.max < knob.min || typeof input.value !== "number") return;
    const value = Math.max(knob.min, Math.min(knob.max, input.value));
    const subdivisions = input.id.startsWith("subdivision:")
        ? grid.subdivisions.map((factor, index) => `subdivision:${index}` === input.id ? value : factor) : grid.subdivisions;
    return readBeatGrid([input.id === "bpm" ? value : beatBpm(grid), input.id === "beatsPerBar" ? value : grid.beatsPerBar, subdivisions]) ?? undefined;
}

export function beatSettingsRows(grid: BeatGrid, sampleRate?: number): PresentationControl[][] {
    const { controls, addAllowed } = beatSettings(grid, sampleRate);
    const rows = controls.map((control): PresentationControl[] => {
        if (control.type !== "knob") return [control];
        const row: PresentationControl[] = [{ type: "text", id: `${control.id}:label`, text: control.label },
            { type: "text", id: `${control.id}:value`, text: String(control.value) }, control];
        if (control.id.startsWith("subdivision:")) row.push({ type: "action", id: `remove:${control.id.split(":")[1]}`, label: "×" });
        return row;
    });
    rows.push([{ type: "action", id: "add", label: "Add subdivision", disabled: !addAllowed }]);
    return rows;
}

export function createBeatsPresentation(): Present {
    let draft: BeatGrid | undefined;
    let displayed: BeatGrid | undefined;
    const present: Present = (snapshot, inputs): PresentationFrame => {
        const time = snapshot.viewport.time;
        const accepted = readBeatGrid(snapshot.node.arguments);
        if (!accepted) {
            displayed = undefined;
            return { status: "Waiting for accepted Beats arguments" };
        }
        if (snapshot.editError || !snapshot.editsPending) draft = undefined;
        let grid = draft ?? accepted;
        const commands: PresentationCommand[] = [];
        let pending: PresentationCommand | undefined;
        for (const input of inputs) {
            if (input.type !== "control") continue;
            if (input.phase === "finish") {
                if (pending) { commands.push(pending); pending = undefined; }
                commands.push({ type: "finish", gesture: input.id });
                continue;
            }
            if (!snapshot.editable) continue;
            const next = editBeatSetting(grid, input, time?.sampleRate);
            if (!next || sameBeatGrid(grid, next)) continue;
            grid = draft = next;
            const event = serializeNodeEvent(SetBeatGrid({ bpm: beatBpm(grid), beats_per_bar: grid.beatsPerBar, subdivisions: [...grid.subdivisions] }));
            const gesture = input.phase === "preview" ? input.id : undefined;
            if (pending && (!gesture || pending.type !== "edit" || pending.gesture !== gesture)) commands.push(pending);
            // SetBeatGrid supplies the whole grid, so a newer preview can
            // replace a pending one within the same knob gesture.
            pending = { type: "edit", event, gesture, replacePending: gesture ? true : undefined };
        }
        if (pending) commands.push(pending);
        displayed = grid;
        const { width, height } = snapshot.viewport;
        const ticks = time?.sampleRate ? beatGridTicks(grid, time, time.sampleRate, width) : [];
        const strengths = gridGuideStrengths(ticks, ({ x }) => x);
        const marks: PresentationMark[] = [];
        let nextLabelX = 0;
        for (const tick of ticks) {
            const y = height * (tick.depth === 0 ? 0 : tick.depth === 1 ? .4 : 1 - .4 / tick.depth);
            marks.push({ id: `beat:${tick.sample}`, type: "polyline", ink: "output", opacity: (strengths.get(tick.depth) ?? 1) * .84,
                points: [{ x: tick.x, y }, { x: tick.x, y: height }] });
            if (snapshot.mode === "editable" && tick.bar !== undefined && tick.x >= nextLabelX && tick.x <= width - 36) {
                marks.push({ id: `label:${tick.sample}`, type: "text", ink: "muted", x: tick.x + 4,
                    y: height - 4, text: String(tick.bar) });
                nextLabelX = tick.x + Math.max(60, 25 + String(tick.bar).length * 7);
            }
        }
        const tempo = `${beatBpm(grid)} BPM`;
        const divisions = grid.subdivisions.length ? `Subdivision ${grid.subdivisions.join(" × ")}` : "No subdivisions";
        return { marks, commands, seekMarks: ticks.map(({ sample }) => String(sample)),
            status: `${tempo} · ${grid.beatsPerBar} beats/bar · ${divisions}`,
            panel: { label: `${beatBpm(grid)} BPM`, rows: beatSettingsRows(grid, time?.sampleRate) } };
    };
    present.openSeekReader = () => {
        const grid = displayed;
        if (!grid) return;
        return ({ first, length, sampleRate, width }) => beatGridTicks(grid,
            { beginSample: BigInt(first), spanSeconds: Number(length) / sampleRate }, sampleRate, width)
            .map(({ sample }) => sample.toString());
    };
    return present;
}

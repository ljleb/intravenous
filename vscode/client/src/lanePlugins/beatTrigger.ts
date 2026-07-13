import { LanePresentationPlugin } from "./protocol";

function renderBeatTriggerLane(context: any): boolean {
    const { lane, content, row, state, timelineStart, scrubToSample, laneWindow } = context;
    const track = document.createElement("div");
    track.className = "beat-track";
    const snapshot = state.uiStateByLaneId[String(lane.laneId)];
    let settings: any = null;
    try { settings = snapshot ? JSON.parse(snapshot.serializedState) : null; } catch (_) {}
    const controls = document.createElement("div");
    controls.className = "beat-controls";
    controls.addEventListener("pointerdown", (event) => event.stopPropagation());
    for (const [key, label] of [["bpm", "BPM"], ["beatsPerBar", "beats per bar"],
        ["beatUnit", "beat unit"], ["eventsPerBeat", "events per beat"]]) {
        const input = document.createElement("input");
        input.type = "number"; input.title = label as string; input.setAttribute("aria-label", label as string);
        input.value = settings && settings[key as string] != null ? String(settings[key as string]) : "";
        input.addEventListener("change", () => {
            if (!settings) return;
            settings[key as string] = Number(input.value);
            context.postLaneUiState(lane.laneId, JSON.stringify(settings), snapshot?.revision);
        });
        controls.appendChild(input);
    }
    track.appendChild(controls);
    // Marker coordinates come only from compiled event-window timestamps.
    for (const event of (content?.events || [])) {
        const x = (Number(event.time) - timelineStart()) / state.samplesPerPixel;
        if (x < 0 || x > track.clientWidth + 1) continue;
        const marker = document.createElement("div"); marker.className = "beat-marker";
        marker.style.left = String(x) + "px"; track.appendChild(marker);
    }
    let scrubPointer: number | null = null;
    track.addEventListener("pointerdown", (event) => {
        scrubPointer = event.pointerId; track.setPointerCapture(event.pointerId);
        scrubToSample(timelineStart() + event.offsetX * state.samplesPerPixel);
    });
    track.addEventListener("pointermove", (event) => {
        if (scrubPointer === event.pointerId) scrubToSample(timelineStart() + event.offsetX * state.samplesPerPixel);
    });
    track.addEventListener("pointerup", (event) => {
        if (scrubPointer === event.pointerId) { scrubPointer = null; track.releasePointerCapture(event.pointerId); }
    });
    row.appendChild(track); laneWindow.appendChild(row);
    return true;
}

const beatTriggerLanePlugin: LanePresentationPlugin = {
    typeId: "iv.timeline.beat-trigger",
    css: ".beat-track{position:relative;flex:1;overflow:hidden;cursor:crosshair;background:var(--vscode-editor-background)}.beat-marker{position:absolute;top:0;bottom:0;width:1px;background:var(--vscode-textLink-foreground);opacity:.75}.beat-controls{display:flex;gap:3px;padding:2px 4px;align-items:center;background:var(--vscode-sideBar-background);z-index:2;position:relative;width:max-content}.beat-controls input{width:52px;font:inherit;color:inherit;background:var(--vscode-input-background);border:1px solid var(--vscode-input-border,transparent)}",
    render: renderBeatTriggerLane,
};

export default beatTriggerLanePlugin;

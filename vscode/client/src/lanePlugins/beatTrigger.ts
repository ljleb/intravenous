import { LanePresentationPlugin } from "./protocol";

function renderBeatTriggerLane(context: any): boolean {
    const { lane, content, row, state, timelineStart, laneWindow, installTimelineControls } = context;
    // Keep the compact lane header intact: it owns lane selection and ports.
    const track = document.createElement("div");
    track.className = "beat-track";
    const snapshot = state.uiStateByLaneId[String(lane.laneId)];
    // Controls keep an independent requested state. They write that state to
    // the backend, but never accept backend snapshots back into their fields.
    // The grid is the sole consumer of server-confirmed output.
    const defaults: any = { bpm: 140, beatsPerBar: 4, beatUnit: 4, eventsPerBeat: 1 };
    let inputSettings: any = { ...defaults };
    let outputSettings: any = { ...defaults };
    try {
        const parsed = snapshot ? JSON.parse(snapshot.serializedState) : null;
        if (parsed && typeof parsed === "object") {
            // Seed a newly created lane view from its current canonical
            // state. Subsequent state notifications update output only.
            inputSettings = { ...inputSettings, ...parsed };
            outputSettings = { ...outputSettings, ...parsed };
        }
    } catch (_) {}
    let outputRevision = Number(snapshot?.revision || 0);
    let outputStateSignature = JSON.stringify(outputSettings);
    const locallyEditedFields = new Set<string>();
    const inputsByField = new Map<string, HTMLInputElement>();
    let publishTimer: ReturnType<typeof setTimeout> | undefined;
    const inputSettingsAreValid = () => Number.isFinite(inputSettings.bpm)
        && inputSettings.bpm >= 1 && inputSettings.bpm <= 1000
        && Number.isInteger(inputSettings.beatsPerBar)
        && inputSettings.beatsPerBar >= 1 && inputSettings.beatsPerBar <= 32
        && Number.isInteger(inputSettings.beatUnit)
        && inputSettings.beatUnit >= 1 && inputSettings.beatUnit <= 64
        && (inputSettings.beatUnit & (inputSettings.beatUnit - 1)) === 0
        && Number.isInteger(inputSettings.eventsPerBeat)
        && inputSettings.eventsPerBeat >= 1 && inputSettings.eventsPerBeat <= 64;
    const publishInputSettings = () => {
        publishTimer = undefined;
        if (!inputSettingsAreValid()) return;
        context.postLaneUiState(lane.laneId, JSON.stringify({
            bpm: inputSettings.bpm,
            beatsPerBar: inputSettings.beatsPerBar,
            beatUnit: inputSettings.beatUnit,
            eventsPerBeat: inputSettings.eventsPerBeat,
        }));
    };
    const scheduleInputPublish = () => {
        if (publishTimer !== undefined) clearTimeout(publishTimer);
        publishTimer = setTimeout(publishInputSettings, 120);
    };
    const controls = document.createElement("div");
    controls.className = "beat-controls";
    controls.addEventListener("pointerdown", (event) => event.stopPropagation());
    const controlGroups: Array<{
        label: string;
        fields: Array<[string, string]>;
        separator?: string;
        suffix?: string;
    }> = [
        { label: "BPM", fields: [["bpm", "beats per minute"]] },
        { label: "Time", fields: [["beatsPerBar", "beats per bar"], ["beatUnit", "beat unit"]], separator: ":" },
        { label: "Events", fields: [["eventsPerBeat", "events per beat"]], suffix: "/ beat" },
    ];
    for (const group of controlGroups) {
        const control = document.createElement("label");
        control.className = "beat-control";
        const label = document.createElement("span");
        label.textContent = group.label;
        control.appendChild(label);
        group.fields.forEach(([key, description], index) => {
            if (index > 0 && group.separator) {
                const separator = document.createElement("span");
                separator.className = "beat-control-separator";
                separator.textContent = group.separator;
                control.appendChild(separator);
            }
            const input = document.createElement("input");
            input.type = "number"; input.title = description; input.setAttribute("aria-label", description);
            input.dataset.beatLaneId = String(lane.laneId);
            input.dataset.beatField = key;
            const limits: Record<string, [number, number]> = {
                bpm: [1, 1000], beatsPerBar: [1, 32], beatUnit: [1, 64], eventsPerBeat: [1, 64],
            };
            input.min = String(limits[key][0]); input.max = String(limits[key][1]);
            input.step = key === "bpm" ? "any" : "1";
            input.value = inputSettings && inputSettings[key] != null ? String(inputSettings[key]) : "";
            inputsByField.set(key, input);
            input.addEventListener("input", () => {
                inputSettings[key] = Number(input.value);
                locallyEditedFields.add(key);
                input.classList.toggle("invalid", !inputSettingsAreValid());
                scheduleInputPublish();
            });
            input.addEventListener("change", () => {
                if (publishTimer !== undefined) clearTimeout(publishTimer);
                publishInputSettings();
            });
            control.appendChild(input);
        });
        if (group.suffix) {
            const suffix = document.createElement("span");
            suffix.className = "beat-control-suffix";
            suffix.textContent = group.suffix;
            control.appendChild(suffix);
        }
        controls.appendChild(control);
    }
    track.appendChild(controls);

    const grid = document.createElement("div");
    grid.className = "beat-event-grid";
    const refreshBeatGrid = () => {
        const interval = Number(outputSettings.eventIntervalSamples);
        const samplesPerPixel = Number(state.samplesPerPixel);
        if (!(interval > 0) || !Number.isFinite(interval) || !(samplesPerPixel > 0)) return;

        const gridStart = timelineStart();
        const periodPixels = interval / samplesPerPixel;
        const eventsPerBeat = Number(outputSettings.eventsPerBeat);
        const eventsPerBar = Number(outputSettings.eventsPerBeat) * Number(outputSettings.beatsPerBar);
        // Show every event while it is readable. Once it is not, retain bar
        // boundaries and recursively remove every other bar. The bar length
        // comes from the lane's time signature, rather than from an arbitrary
        // fixed number of beats.
        let eventStride = 1;
        if (periodPixels < 12 && Number.isInteger(eventsPerBar) && eventsPerBar > 0) {
            eventStride = eventsPerBar;
            while (periodPixels * eventStride < 12) eventStride *= 2;
        }

        grid.style.removeProperty("--beat-period");
        grid.style.removeProperty("--beat-phase");
        grid.style.removeProperty("--beat-first-event-x");
        const eventTime = (markerIndex: number) => Math.round(markerIndex * eventStride * interval);
        let eventIndex = Math.max(0, Math.ceil(gridStart / (eventStride * interval)));
        while (eventIndex > 0 && eventTime(eventIndex - 1) >= gridStart) --eventIndex;
        while (eventTime(eventIndex) < gridStart) ++eventIndex;
        const end = gridStart + grid.clientWidth * samplesPerPixel;
        const markers = document.createDocumentFragment();
        for (; eventTime(eventIndex) <= end; ++eventIndex) {
            const time = eventTime(eventIndex);
            const marker = document.createElement("div");
            marker.className = "beat-marker";
            const sourceEventIndex = eventIndex * eventStride;
            if (sourceEventIndex % eventsPerBar === 0) {
                marker.classList.add("bar");
            } else if (sourceEventIndex % eventsPerBeat === 0) {
                marker.classList.add("beat");
            } else {
                marker.classList.add("subdivision");
            }
            marker.style.left = String((time - gridStart) / samplesPerPixel) + "px";
            marker.title = "trigger at sample " + String(time);
            markers.appendChild(marker);
        }
        grid.replaceChildren(markers);
    };
    (grid as any).__ivRefreshBeats = refreshBeatGrid;
    (grid as any).__ivApplyBeatSettings = (nextSettings: any, revision: unknown) => {
        if (!nextSettings || typeof nextSettings !== "object") return;
        const nextRevision = Number(revision);
        const nextSignature = JSON.stringify(nextSettings);
        if (Number.isFinite(nextRevision)) {
            if (nextRevision < outputRevision) return false;
            if (nextRevision === outputRevision && nextSignature === outputStateSignature) return false;
            outputRevision = Math.max(outputRevision, nextRevision);
        }
        outputSettings = { ...outputSettings, ...nextSettings };
        outputStateSignature = nextSignature;
        // A server notification is shared by every open lane view. Mirror it
        // into untouched fields, while leaving the originating view's local
        // edit in place until the notification acknowledges that exact value.
        for (const [field, input] of inputsByField) {
            const nextValue = nextSettings[field];
            if (nextValue == null) continue;
            if (locallyEditedFields.has(field)) {
                if (Number(nextValue) === Number(inputSettings[field])) {
                    locallyEditedFields.delete(field);
                }
                continue;
            }
            inputSettings[field] = nextValue;
            input.value = String(nextValue);
            input.classList.remove("invalid");
        }
        refreshBeatGrid();
        return true;
    };
    track.appendChild(grid);
    installTimelineControls?.(grid, timelineStart, false);
    row.appendChild(track); laneWindow.appendChild(row);
    // Rows are initially assembled detached, so clientWidth becomes usable on
    // the next frame for the precise, sparse-marker path.
    requestAnimationFrame(refreshBeatGrid);
    return true;
}

const beatTriggerLanePlugin: LanePresentationPlugin = {
    typeId: "iv.timeline.beat-trigger",
    css: ".beat-track{position:relative;display:flex;flex:1 1 auto;min-width:180px;min-height:0;flex-direction:column;overflow:hidden;background:var(--vscode-editor-background)}.beat-controls{display:flex;flex:0 0 auto;gap:8px;padding:2px 4px;align-items:center;background:var(--vscode-sideBar-background);border-bottom:1px solid var(--vscode-sideBarSectionHeader-border,rgba(128,128,128,.18));position:relative;z-index:2;width:max-content}.beat-control{display:flex;align-items:center;gap:3px;color:var(--vscode-descriptionForeground);font-size:.82em;white-space:nowrap}.beat-control-separator{font-size:1.25em;color:var(--vscode-foreground);line-height:1}.beat-control-suffix{font-size:.9em}.beat-controls input{width:48px;font:inherit;color:inherit;background:var(--vscode-input-background);border:1px solid var(--vscode-input-border,transparent);cursor:ns-resize}.beat-controls input:focus{cursor:text}.beat-controls input.invalid{border-color:var(--vscode-inputValidation-errorBorder,var(--vscode-errorForeground))}.beat-event-grid{position:relative;flex:1 1 auto;min-height:0;overflow:hidden;cursor:default;background:var(--vscode-editor-background)}.beat-marker{position:absolute;top:0;bottom:0;width:1px;background:var(--vscode-editorWidget-border,rgba(220,220,220,.55));pointer-events:none}.beat-marker.bar{width:3px;opacity:1}.beat-marker.beat{opacity:.9}.beat-marker.subdivision{opacity:.42}",
    render: renderBeatTriggerLane,
};

export default beatTriggerLanePlugin;

import { LanePresentationPlugin } from "./protocol";

function renderBeatTriggerLane(context: any): boolean {
    const { lane, content, row, state, timelineStart, scrubToSample, laneWindow, postDebug } = context;
    const pointerDebug = (phase: string, field = "", detail = "") => {
        postDebug({ type: "beatPointerDebug", laneId: String(lane.laneId), field, phase, detail });
    };
    // Keep the compact lane header intact: it owns lane selection and ports.
    const track = document.createElement("div");
    track.className = "beat-track";
    const gridStart = timelineStart();
    const snapshot = state.uiStateByLaneId[String(lane.laneId)];
    // A usable local model makes the controls responsive even while the
    // initial UI-state notification is still in flight.
    let settings: any = { bpm: 140, beatsPerBar: 4, beatUnit: 4, eventsPerBeat: 1 };
    try {
        const parsed = snapshot ? JSON.parse(snapshot.serializedState) : null;
        if (parsed && typeof parsed === "object") settings = { ...settings, ...parsed };
    } catch (_) {}
    let publishTimer: ReturnType<typeof setTimeout> | undefined;
    const settingsAreValid = () => Number.isFinite(settings.bpm) && settings.bpm >= 1 && settings.bpm <= 1000
        && Number.isInteger(settings.beatsPerBar) && settings.beatsPerBar >= 1 && settings.beatsPerBar <= 32
        && Number.isInteger(settings.beatUnit) && settings.beatUnit >= 1 && settings.beatUnit <= 64
        && (settings.beatUnit & (settings.beatUnit - 1)) === 0
        && Number.isInteger(settings.eventsPerBeat) && settings.eventsPerBeat >= 1 && settings.eventsPerBeat <= 64;
    const publishSettings = () => {
        publishTimer = undefined;
        if (!settingsAreValid()) return;
        // Do not retain an old revision while rapidly editing a number. The
        // backend's canonical snapshot returns the new revision afterward.
        context.postLaneUiState(lane.laneId, JSON.stringify(settings));
    };
    const schedulePublish = () => {
        if (publishTimer !== undefined) clearTimeout(publishTimer);
        publishTimer = setTimeout(publishSettings, 120);
    };
    const documentWithBeatDrag = document as any;
    const beatDrag = documentWithBeatDrag.__ivBeatControlDrag || (documentWithBeatDrag.__ivBeatControlDrag = {
        active: null,
        installed: false,
    });
    if (!beatDrag.installed) {
        beatDrag.installed = true;
        const applyMovement = (event: MouseEvent | PointerEvent) => {
            const drag = beatDrag.active;
            if (!drag) return;
            const locked = document.pointerLockElement === drag.lockElement;
            if (!locked && (event.clientY !== drag.lastClientY)) {
                requestPointerLock(drag);
            }
            const deltaY = locked ? event.movementY : event.clientY - drag.lastClientY;
            if (!locked) drag.lastClientY = event.clientY;
            if (deltaY !== 0) {
                drag.apply(deltaY);
                const now = Date.now();
                if (now - drag.lastDebugAt >= 100) {
                    drag.lastDebugAt = now;
                    drag.debug("move", "deltaY=" + String(deltaY) + " " + (locked ? "locked" : "unlocked")
                        + " value=" + String(drag.input.value));
                }
            }
        };
        const requestPointerLock = (drag: any) => {
            if (drag.pointerLockRequested || typeof drag.lockElement.requestPointerLock !== "function") return;
            drag.pointerLockRequested = true;
            drag.debug("lock-request", "pointer=" + String(drag.pointerId));
            try { drag.lockElement.requestPointerLock(); } catch (_) {
                drag.pointerLockRequested = false;
                drag.debug("lock-error");
            }
        };
        document.addEventListener("pointermove", applyMovement);
        document.addEventListener("mousemove", (event) => {
            // Pointer events cover the unlocked case. This fallback is only
            // for pointer-locked movement (or browsers without PointerEvent),
            // so one physical move cannot be applied twice.
            if (window.PointerEvent && document.pointerLockElement == null) return;
            applyMovement(event);
        });
        const finishDrag = (event?: PointerEvent | MouseEvent) => {
            const drag = beatDrag.active;
            if (!drag) return;
            const isPointerEvent = typeof PointerEvent !== "undefined" && event instanceof PointerEvent;
            if (isPointerEvent && (event as PointerEvent).pointerId !== drag.pointerId) return;
            if (event instanceof MouseEvent && !isPointerEvent && window.PointerEvent) return;
            drag.debug("end", "pointer=" + String(drag.pointerId) + " value=" + String(drag.input.value));
            beatDrag.active = null;
            try { drag.lockElement.releasePointerCapture(drag.pointerId); } catch (_) {}
            if (document.pointerLockElement === drag.lockElement) {
                try { document.exitPointerLock(); } catch (_) {}
            }
            drag.finish();
            document.dispatchEvent(new Event("ivBeatDragEnded"));
        };
        document.addEventListener("pointerup", finishDrag);
        document.addEventListener("mouseup", finishDrag);
        document.addEventListener("pointercancel", finishDrag);
        document.addEventListener("pointerlockchange", () => {
            const drag = beatDrag.active;
            if (!drag) return;
            drag.debug(document.pointerLockElement === drag.lockElement ? "lock-acquired" : "lock-released");
            if (document.pointerLockElement !== drag.lockElement) drag.pointerLockRequested = false;
        });
    }
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
            input.value = settings && settings[key] != null ? String(settings[key]) : "";
            input.addEventListener("input", () => {
                settings[key] = Number(input.value);
                input.classList.toggle("invalid", !settingsAreValid());
                schedulePublish();
            });
            input.addEventListener("change", () => {
                if (publishTimer !== undefined) clearTimeout(publishTimer);
                publishSettings();
            });
            input.addEventListener("pointerdown", (event) => {
                if (event.button !== 0) return;
                // Do not prevent the native click: releasing without a drag
                // must leave this number input focused so it can be typed.
                const startValue = Number(input.value) || 0;
                let totalDeltaY = 0;
                let dragging = false;
                const sequences: Record<string, number[]> = {
                    beatsPerBar: Array.from({ length: 32 }, (_, value) => value + 1),
                    beatUnit: [1, 2, 4, 8, 16, 32, 64],
                    eventsPerBeat: Array.from({ length: 64 }, (_, value) => value + 1),
                };
                const sequence = sequences[key];
                const initialSequenceIndex = sequence
                    ? Math.max(0, sequence.indexOf(startValue)) : 0;
                const apply = (deltaY: number) => {
                    totalDeltaY += deltaY;
                    // Ignore the tiny movement that commonly accompanies a
                    // click. Once this threshold is crossed it is a drag.
                    if (!dragging && Math.abs(totalDeltaY) < 3) return;
                    dragging = true;
                    let value: number;
                    if (key === "bpm") {
                        value = Math.round(startValue - totalDeltaY);
                    } else if (sequence) {
                        // Time-signature fields (and event density) advance
                        // through a discrete, precomputed sequence. Deriving
                        // the index from total motion prevents small pointer
                        // events from cancelling one another.
                        const index = initialSequenceIndex - Math.round(totalDeltaY / 6);
                        value = sequence[Math.max(0, Math.min(sequence.length - 1, index))];
                    } else {
                        value = Math.round(startValue - totalDeltaY / 6);
                    }
                    const [minimum, maximum] = limits[key];
                    const nextValue = String(Math.max(minimum, Math.min(maximum, value)));
                    if (input.value !== nextValue) {
                        input.value = nextValue;
                        input.dispatchEvent(new Event("input"));
                    }
                };
                beatDrag.active = {
                    lockElement: input,
                    pointerId: event.pointerId,
                    lastClientY: event.clientY,
                    lastDebugAt: 0,
                    pointerLockRequested: false,
                    input,
                    debug: (phase: string, detail = "") => pointerDebug(phase, key, detail),
                    apply,
                    finish: () => {
                        if (dragging) input.dispatchEvent(new Event("change"));
                    },
                };
                pointerDebug("down", key, "pointer=" + String(event.pointerId) + " value=" + String(input.value));
                try { input.setPointerCapture(event.pointerId); } catch (_) {}
                requestPointerLock(beatDrag.active);
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

    // The backend supplies this lane's compiled event output for the visible
    // window.  Do not measure clientWidth here: rows are rendered detached,
    // and doing so previously discarded almost every marker.
    let previousMarkerX = -Infinity;
    for (const event of (content?.events || [])) {
        const x = (Number(event.time) - gridStart) / state.samplesPerPixel;
        // Like ruler graduations, do not draw marks that are too close to be
        // independently useful at this zoom level.
        if (x - previousMarkerX < 12) continue;
        previousMarkerX = x;
        const marker = document.createElement("div"); marker.className = "beat-marker";
        marker.style.left = String(x) + "px"; marker.title = "trigger at sample " + String(event.time);
        grid.appendChild(marker);
    }
    track.appendChild(grid);
    let scrubPointer: number | null = null;
    grid.addEventListener("pointerdown", (event) => {
        // Keep secondary-click available to the lane's default context menu.
        // Previously it also scrubbed the beat grid before that menu could
        // open, which made this lane effectively impossible to rename.
        if (event.button !== 0) return;
        scrubPointer = event.pointerId; grid.setPointerCapture(event.pointerId);
        scrubToSample(gridStart + event.offsetX * state.samplesPerPixel);
    });
    grid.addEventListener("pointermove", (event) => {
        if (scrubPointer === event.pointerId) scrubToSample(gridStart + event.offsetX * state.samplesPerPixel);
    });
    grid.addEventListener("pointerup", (event) => {
        if (scrubPointer === event.pointerId) { scrubPointer = null; grid.releasePointerCapture(event.pointerId); }
    });
    row.appendChild(track); laneWindow.appendChild(row);
    return true;
}

const beatTriggerLanePlugin: LanePresentationPlugin = {
    typeId: "iv.timeline.beat-trigger",
    css: ".beat-track{position:relative;display:flex;flex:1 1 auto;min-width:180px;min-height:0;flex-direction:column;overflow:hidden;background:var(--vscode-editor-background)}.beat-controls{display:flex;flex:0 0 auto;gap:8px;padding:2px 4px;align-items:center;background:var(--vscode-sideBar-background);border-bottom:1px solid var(--vscode-sideBarSectionHeader-border,rgba(128,128,128,.18));position:relative;z-index:2;width:max-content}.beat-control{display:flex;align-items:center;gap:3px;color:var(--vscode-descriptionForeground);font-size:.82em;white-space:nowrap}.beat-control-separator{font-size:1.25em;color:var(--vscode-foreground);line-height:1}.beat-control-suffix{font-size:.9em}.beat-controls input{width:48px;font:inherit;color:inherit;background:var(--vscode-input-background);border:1px solid var(--vscode-input-border,transparent);cursor:ns-resize}.beat-controls input:focus{cursor:text}.beat-controls input.invalid{border-color:var(--vscode-inputValidation-errorBorder,var(--vscode-errorForeground))}.beat-event-grid{position:relative;flex:1 1 auto;min-height:0;overflow:hidden;cursor:default;background:var(--vscode-editor-background)}.beat-marker{position:absolute;top:0;bottom:0;width:1px;background:var(--vscode-editorWidget-border,rgba(220,220,220,.55));opacity:.9;pointer-events:none}",
    render: renderBeatTriggerLane,
};

export default beatTriggerLanePlugin;

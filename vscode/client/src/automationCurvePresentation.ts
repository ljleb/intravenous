import { automationBoundTracksForSpans, automationBoundVisiblePositions, sampleAutomationPreview } from "./automationBoundInputs";
import { AutomationCurveEditing, automationTensionLimits, readAutomationCurve, type CurveAxes, type CurveDragKind } from "./automationCurveEditing";
import {
    automationSampleOffset, automationValueFraction, automationValueGuides, automationWindow, compareAutomationTime,
    panAutomationValueAxis, panCurvedAutomationValueZoom, rankAutomationValueGuides, readAutomationCurveView, zoomAutomationValueAxis,
    type AutomationCurve, type AutomationCurveView, type AutomationPointPosition, type CurveInterpolation,
} from "./automationCurveGeometry";
import { gridGuideStrengths } from "./gridGuideStrength";
import { PresentationEditClock, PresentationEditRequests } from "./presentationEditRequests";
import type {
    Present, PresentationCommand, PresentationControl, PresentationFrame, PresentationGestureInput, PresentationInk,
    PresentationManifest, PresentationMark, PresentationPoint, PresentationSavedState, PresentationSnapshot, PresentationTarget,
} from "./presentationPlugin";
import { sampleWindow } from "./sampleWaveform";
import { scrubSensitivityShortcut } from "./scrubSensitivity";
import { automationTimelineTicks, snapTimelineSample, timelineSnapRadiusPx } from "./timelineGeometry";

const definitionId = "builtin.automation.curve";
export const automationCurvePresentationManifest: PresentationManifest = {
    version: 1, id: definitionId, title: "Automation curve", definitionIds: [definitionId], timeline: true, timelineSnapping: true,
    icon: "M2 12C5 12 5 4 14 4M2 10v4m12-12v4",
    observations: (["min", "max"] as const).map((port) => ({
        id: port, direction: "input", port, points: automationBoundVisiblePositions, whenConnected: true, completeOnly: true,
    })),
};

const rulerOptions = [["auto", "Auto"], ["whole", "Whole"], ["half", "Halves"], ["off", "Off"]];
const displayOptions = [["curved", "Curved"], ["cropped", "Cropped"]];
const segmentOptions = [["empty", "Empty"], ["hold", "Hold"], ["bezier", "Bézier"]];
const pointShortcuts = "⇧Drag Keep type · ⇧Click Delete · Right-click Segment";
function choice(id: string, label: string, value: string, options: string[][]): PresentationControl[] {
    return [{ type: "choice", id, label, value, options: options.map(([value, label]) => ({ value, label })) }];
}

function pointStatus(point: AutomationPointPosition): string {
    const time = point.sampleIndex !== undefined ? `${point.sampleIndex} smp` : `${Number(point.seconds.toPrecision(6))} s`;
    return `${time} · Value ${Number(point.value.toPrecision(6))}`
        + (point.tension === undefined ? "" : ` · Tension ${Number(point.tension.toFixed(2))}`);
}

function windowFor(curve: AutomationCurve, viewport: PresentationSnapshot["viewport"]) {
    const first = curve.points[0]?.seconds ?? 0;
    const last = curve.points.at(-1)?.seconds ?? first;
    const beginSeconds = viewport.time?.beginSeconds ?? (last > first ? first : first - .5);
    const spanSeconds = viewport.time?.spanSeconds ?? (last > first ? last - first : 1);
    return { beginSeconds, endSeconds: beginSeconds + spanSeconds, spanSeconds };
}

function traceMarks(marks: PresentationMark[], name: string, tracks: readonly { seconds: number; value: number }[][],
    project: (seconds: number, value: number) => PresentationPoint, ink: PresentationInk, opacity: number, dashed = false) {
    const budget = 2400;
    const runs = tracks.filter((track) => track.length >= 2);
    // Reserve the endpoints of each displayed run before distributing any
    // remaining vertices. Dense data still spans both sides of the viewport.
    const countRuns = Math.min(runs.length, 256);
    const displayed = Array.from({ length: countRuns }, (_, i) => runs[Math.floor(i * runs.length / countRuns)]);
    const interior = displayed.reduce((sum, track) => sum + track.length - 2, 0);
    let spent = 0;
    let seen = 0;
    for (const [i, track] of displayed.entries()) {
        seen += track.length - 2;
        const total = interior ? Math.min(seen, Math.floor(seen * (budget - 2 * countRuns) / interior)) : 0;
        const count = 2 + total - spent;
        spent = total;
        const points = Array.from({ length: count }, (_, at) => {
            const { seconds, value } = track[Math.round(at * (track.length - 1) / (count - 1))];
            return project(seconds, value);
        });
        marks.push({ type: "polyline", id: `${name}:${i}`, points, ink, opacity, dashed: dashed || undefined,
            strokeWidth: dashed ? 1 : 1.5 });
    }
}

function axisGeometry(curve: AutomationCurve, snapshot: PresentationSnapshot, axes: CurveAxes) {
    const { width, height } = snapshot.viewport;
    const x = (offset: number) => offset / (axes.spanSeconds ?? 1) * width;
    const sampleX = (index: bigint) => curve.sampleRate === undefined ? undefined
        : x(automationSampleOffset(index, axes.begin, curve.sampleRate));
    const pointX = (point: AutomationPointPosition) => (point.sampleIndex === undefined ? undefined : sampleX(point.sampleIndex))
        ?? x(point.seconds - axes.begin);
    const y = (value: number) => (1 - automationValueFraction(value, axes)) * height;
    const marker = (point: AutomationPointPosition) => ({ x: pointX(point), y: Math.max(2, Math.min(height - 2, y(point.value))) });
    return { x, y, sampleX, marker };
}

export function createAutomationCurvePresentation(): Present {
    const clock = new PresentationEditClock();
    const requests = new PresentationEditRequests();
    const edits = new AutomationCurveEditing({ send: (_, event) => requests.send(event), gesture: () => requests.gesture() }, () => {}, clock);
    let previous: { revision: number; rate?: number; args: unknown[]; address: PresentationSnapshot["node"]["address"] } | undefined;
    let saved: PresentationSavedState | undefined;
    let view: AutomationCurveView = {};
    let menu: number | undefined;
    let editingTension = false;
    let nextPointer = 0;
    let pointer: { id: number; kind: CurveDragKind } | undefined;
    let marquee: { first: PresentationPoint; last: PresentationPoint; modifiers: PresentationGestureInput["modifiers"] } | undefined;

    return (snapshot, inputs): PresentationFrame => {
        const { address, arguments: args } = snapshot.node;
        const { width, height, time } = snapshot.viewport;
        clock.setTime(snapshot.clockMs ?? 0);
        const rate = time?.sampleRate;
        const construction = { definitionId: snapshot.node.definitionId, arguments: args };
        if (!previous || previous.args !== args || previous.address !== address || previous.revision !== snapshot.revision || previous.rate !== rate) {
            edits.reconcile({ sessionRevision: snapshot.revision, sampleRate: rate, nodes: address ? [{ address, construction }] : [] });
            previous = { revision: snapshot.revision, rate, args, address };
        }
        if (saved !== snapshot.state) { view = readAutomationCurveView(snapshot.state); saved = snapshot.state; }
        let continuation = false;
        for (const input of inputs) if (input.type === "editResult") {
            requests.receive(input);
            continuation = true;
        }
        const curve = address ? edits.view(address) : readAutomationCurve(construction, rate);
        const editable = !!address && snapshot.editable && !!curve?.sampleRate;
        if (!editable && edits.pointerId() !== null) {
            edits.abandon();
            editingTension = false;
            continuation = true;
        }
        let cancelDrag = pointer !== undefined && edits.pointerId() !== pointer.id;
        if (cancelDrag) pointer = undefined;
        const commands: PresentationCommand[] = [];
        let stateChanged = false;
        const updateView = (next: AutomationCurveView) => {
            const state = readAutomationCurveView(next);
            if (JSON.stringify(state) !== JSON.stringify(view)) { view = state; stateChanged = true; }
        };
        let cursor: PresentationPoint | undefined;
        if (!curve) return { status: "Waiting for accepted Automation Curve arguments", cancelDrag: cancelDrag || undefined,
            commands: requests.takeCommands(), wakeAfterMs: continuation ? 0 : clock.nextDelay() };

        const window = windowFor(curve, snapshot.viewport);
        const axes = (): CurveAxes => {
            // Hold the value scale while edits are pending. Time still follows the viewport.
            const frozen = address && edits.axes(address);
            return { left: 0, top: 0, width, height, begin: window.beginSeconds, end: window.endSeconds, spanSeconds: window.spanSeconds,
                pan: frozen ? frozen.pan : view.pan, zoom: frozen ? frozen.zoom : view.zoom,
                display: frozen?.display ?? view.display, ruler: frozen ? frozen.ruler : view.ruler };
        };
        const pointView = () => automationWindow(curve, 0, window.spanSeconds, address ? edits.drafts(address) : [], window.beginSeconds);
        const geometry = () => axisGeometry(curve, snapshot, axes());
        const visible = (points: ReturnType<typeof pointView>["points"], axis: CurveAxes) => points.filter((point) => {
            if (axis.display !== "cropped" && !axis.zoom) return true;
            const fraction = automationValueFraction(point.value, axis);
            return fraction >= 0 && fraction <= 1;
        });
        const area = () => marquee && ({
            x: Math.min(marquee.first.x, marquee.last.x), y: Math.min(marquee.first.y, marquee.last.y),
            width: Math.abs(marquee.first.x - marquee.last.x), height: Math.abs(marquee.first.y - marquee.last.y),
        });
        const bounded = ({ x, y }: PresentationPoint) => ({ x: Math.max(0, Math.min(width, x)), y: Math.max(0, Math.min(height, y)) });
        const selectedPoint = () => menu === undefined ? undefined : curve.points.find(({ id }) => id === menu);
        const processDrag = (input: PresentationGestureInput) => {
            if (!address) return;
            if (input.phase === "finish" || input.phase === "cancel") continuation = true;
            if (input.id === "selection") {
                if (input.phase === "begin") {
                    if (edits.pointerId() !== null) { cancelDrag = true; return; }
                    const first = bounded(input.point);
                    marquee = { first, last: first, modifiers: input.modifiers };
                } else if (marquee) {
                    marquee.last = bounded(input.point);
                    if (input.phase === "finish") {
                        const box = area()!, modifiers = marquee.modifiers;
                        const { marker } = geometry();
                        const ids = Math.hypot(box.width, box.height) < 3 ? [] : visible(pointView().points, axes()).filter((point) => {
                            const { x, y } = marker(point);
                            return x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height;
                        }).map(({ id }) => id);
                        edits.selectPoints(address, ids, { ctrlKey: modifiers.control, metaKey: modifiers.meta, shiftKey: modifiers.shift });
                    }
                    if (input.phase === "finish" || input.phase === "cancel") marquee = undefined;
                }
                return;
            }
            if (input.phase === "begin") {
                const kind = input.id.startsWith("tension:") ? "tension" : "point";
                const id = input.id === "add" ? null : Number(input.id.split(":")[1]);
                const marks = input.snapping?.marks.map(BigInt) ?? [];
                const playhead = input.snapping?.playhead;
                const tolerance = timelineSnapRadiusPx * (rate ?? 0) * window.spanSeconds / Math.max(1, width);
                const pointerAxes = { ...axes(), timeDetents: playhead === undefined ? marks : [...marks, BigInt(playhead)],
                    snapTime: (index: bigint) => snapTimelineSample(index, marks, tolerance) };
                const pointerId = ++nextPointer;
                if (edits.begin(address, id, pointerId, pointerAxes, input.point, kind, input.modifiers.shift))
                    pointer = { id: pointerId, kind };
                else cancelDrag = true;
                menu = undefined;
            } else if (pointer) {
                switch (input.phase) {
                    case "move": edits.move(pointer.id, input.point); break;
                    case "modifiers": edits.setPreservePointType(input.modifiers.shift); break;
                    case "wheel": edits.changeSensitivity(input.wheel ?? 0); break;
                    case "finish":
                    case "cancel": {
                        const point = edits.activePoint();
                        if (input.phase === "finish" && pointer.kind === "point" && point) cursor = geometry().marker(point);
                        pointer = undefined;
                        void edits.finish(input.phase === "finish" && input.modifiers.shift);
                        break;
                    }
                }
            }
        };
        for (const input of inputs) {
            if (input.type === "interrupt") {
                if (input.reason === "disconnected") {
                    edits.disconnected();
                    previous = undefined;
                } else edits.abandon();
                cancelDrag ||= pointer !== undefined || marquee !== undefined;
                pointer = undefined; marquee = undefined; menu = undefined; editingTension = false;
                continuation = true;
                continue;
            }
            if (input.type === "dismissContext") {
                if (editingTension) {
                    if (input.cancelled) edits.abandon();
                    else void edits.finish();
                    continuation = true;
                }
                menu = undefined;
                editingTension = false;
                continue;
            }
            if (input.type === "valueView" && address && edits.pointerId() === null && !marquee) {
                if (input.action === "reset") updateView({ ...view, pan: undefined, zoom: undefined });
                else if (input.action === "zoom") {
                    const next = zoomAutomationValueAxis(view, input.factor, 1 - input.y);
                    updateView({ ...view, ...next });
                } else if (view.display !== "cropped" && view.zoom && view.zoom.factor > 1)
                    updateView({ ...view, zoom: panCurvedAutomationValueZoom(view.zoom, input.delta, height) });
                else updateView({ ...view, pan: panAutomationValueAxis(view, input.delta, height) });
            }
            if (!editable || !address) continue;
            if (input.type === "gesture") processDrag(input);
            else if (input.type === "context") menu = Number(input.id.split(":")[1]);
            else if (input.type === "items") {
                if (input.action === "selectAll") edits.selectPoints(address, [...curve.points, ...edits.drafts(address)].map(({ id }) => id));
                else void edits.removeSelection(address);
            } else if (input.type === "control") {
                const point = selectedPoint();
                if (input.id === "display" && displayOptions.some(([id]) => id === input.value))
                    updateView({ ...view, display: input.value === "cropped" ? "cropped" : "curved", pan: undefined, zoom: undefined });
                else if (input.id === "ruler" && rulerOptions.some(([id]) => id === input.value))
                    updateView({ ...view, ruler: input.value as AutomationCurveView["ruler"] });
                else if (point && input.id === "segment" && segmentOptions.some(([id]) => id === input.value))
                    void edits.segment(address, point.id, input.value as CurveInterpolation, point.tension);
                else if (point && input.id === "remove") { void edits.remove(address, point.id); menu = undefined; }
                else if (input.id === "tension") {
                    if (input.phase === "finish" && editingTension) {
                        void edits.finish();
                        editingTension = false;
                        continuation = true;
                    }
                    else if (point && typeof input.value === "number") {
                        if (input.phase === "commit") void edits.segment(address, point.id, "bezier", Math.fround(input.value));
                        else {
                            if (edits.pointerId() === null)
                                editingTension = edits.begin(address, point.id, ++nextPointer, axes(), { x: 0, y: 0 }, "tension");
                            if (editingTension) edits.setDraggedTension(address, point.id, input.value);
                        }
                    }
                }
            }
        }
        clock.runDue();

        const axis = axes();
        const { x, y, sampleX, marker } = axisGeometry(curve, snapshot, axis);
        const tile = sampleWindow(window.beginSeconds, window.endSeconds, rate, automationBoundVisiblePositions);
        const track = (port: "min" | "max") => {
            const spans = snapshot.observations[port]?.spans;
            return tile && rate && spans ? automationBoundTracksForSpans(spans.map(({ begin, end, result }) =>
                ({ begin: BigInt(begin), end: BigInt(end), result })), tile, window.beginSeconds, window.spanSeconds, rate) : [];
        };
        const manual = (port: string) => snapshot.node.ports.find((p) => p.direction === "input" && p.name === port && !p.connected)?.manualValue;
        const geometryInView = pointView();
        const preview = sampleAutomationPreview(geometryInView.paths, track("min"), track("max"),
            { min: manual("min"), max: manual("max") }, window.spanSeconds, width);
        const marks: PresentationMark[] = [];
        const targets: PresentationTarget[] = [];
        const line = (id: string, from: PresentationPoint, to: PresentationPoint, ink: PresentationInk, opacity: number,
            fade?: { from: PresentationPoint; to: PresentationPoint }) => marks.push({ type: "polyline", id, points: [from, to], ink, opacity, fade });
        const label = (id: string, text: string, x: number, y: number, ink: PresentationInk = "muted") =>
            marks.push({ type: "text", id, text, x, y, ink });
        const labels = snapshot.mode !== "nano" && (snapshot.mode !== "compact" || height > 16);
        const guides = rankAutomationValueGuides(automationValueGuides(axis, height, axis.ruler)).slice(0, 100);
        const strengths = gridGuideStrengths(guides, ({ y }) => y);
        const inset = labels && guides.length ? 9 + 6 * Math.max(...guides.map(({ value }) => String(value).length)) : 0;
        const guideLabelY = (position: number) => position >= 12 ? position - 3 : Math.min(height - 1, position + 10);
        if (!time?.gridMode || time.gridMode === "seconds") {
            const ticks = automationTimelineTicks(window, width, rate);
            const count = Math.min(240, ticks.length);
            for (let i = 0; i < count; ++i) {
                const index = count > 1 ? Math.round(i * (ticks.length - 1) / (count - 1)) : 0;
                const tick = ticks[index];
                line(`time:${index}`, { x: tick.x, y: 0 }, { x: tick.x, y: height }, "foreground", tick.minor ? .09 : .3);
                if (!labels) continue;
                const labelX = index === ticks.length - 1 && tick.x >= width - 48
                    ? Math.max(inset, tick.x - 3 - Math.max(tick.label.length, tick.sampleLabel?.length ?? 0) * 6)
                    : Math.max(inset, tick.x) + 3;
                if (!tick.minor) label(`time-label:${index}`, tick.label, labelX, 10);
                if (tick.sampleLabel) label(`time-samples:${index}`, tick.sampleLabel, labelX, 21);
            }
        }
        const valueSnap = address && editable ? edits.activeSnap(address) : null;
        guides.forEach((guide, i) => {
            line(`value:${i}`, { x: 0, y: guide.y }, { x: width, y: guide.y }, "foreground", .15 * (strengths.get(guide.depth) ?? 1));
            if (labels && guide.value !== valueSnap?.value) label(`value-label:${i}`, String(guide.value), 4, guideLabelY(guide.y));
        });
        const project = (seconds: number, value: number) => ({ x: x(seconds), y: y(value) });
        const boundedOutput = preview.min.length || preview.max.length;
        traceMarks(marks, "authored", preview.authored, project, "input", boundedOutput ? .32 : 1);
        if (boundedOutput) traceMarks(marks, "output", preview.output, project, "input", 1);
        traceMarks(marks, "min", preview.min, project, "highlight", .55, true);
        traceMarks(marks, "max", preview.max, project, "warning", .55, true);

        if (valueSnap) {
            const at = marker(valueSnap.point), snapY = y(valueSnap.value);
            if (snapY >= 0 && snapY <= height) {
                line("value-snap-guide", { x: 0, y: snapY }, { x: width, y: snapY }, "highlight", .35);
                line("value-snap", { x: 0, y: snapY }, { x: width, y: snapY }, "highlight", 1,
                    { from: { x: at.x - 144, y: snapY }, to: { x: at.x + 144, y: snapY } });
                if (labels) label("value-snap-label", String(valueSnap.value), 4, guideLabelY(snapY), "highlight");
            }
        }
        const timeSnap = address && editable ? edits.activeTimeSnap(address) : null;
        const sx = timeSnap ? sampleX(timeSnap.sampleIndex) : undefined;
        if (timeSnap && sx !== undefined && sx >= 0 && sx <= width) {
            const at = marker(timeSnap.point);
            line("time-snap", { x: sx, y: 0 }, { x: sx, y: height }, "highlight", 1,
                { from: { x: sx, y: at.y - 144 }, to: { x: sx, y: at.y + 144 } });
        }

        const selection = area();
        if (selection) marks.push({ type: "rect", id: "selection", ...selection, ink: "selection", opacity: .25, border: "selection" });
        if (editable && address) {
            targets.push({ id: "selection", x: 0, y: 0, width, height, drag: "absolute", hint: "Drag to select · Right-drag to add" },
                { id: "add", x: 0, y: 0, width, height, button: "secondary", wheel: true, hideCursor: true, cursorReturn: true });
            const selected = edits.selectedPoints(address);
            const points = visible(geometryInView.points, axis);
            const displayedCount = Math.min(490, points.length);
            const indices = new Set(Array.from({ length: displayedCount }, (_, i) => Math.floor(i * points.length / displayedCount)));
            const activeIndex = pointer ? points.findIndex(({ id }) => id === edits.activePoint()?.id) : -1;
            if (activeIndex >= 0) indices.add(activeIndex);
            const target = (id: string, at: PresentationPoint, radius: number) => ({
                id, x: at.x - radius, y: at.y - radius, width: radius * 2, height: radius * 2, context: true as const, hideCursor: true as const,
            });
            for (const i of indices) {
                const point = points[i], at = marker(point);
                if (point.interpolation !== "bezier" || (points[i + 1] && compareAutomationTime(point, points[i + 1]) === 0)) continue;
                const handle = { x: at.x, y: at.y + (at.y < height / 2 ? 15 : -15) };
                const id = `tension:${point.id}`;
                targets.push({ ...target(id, handle, 6), hint: `Tension ${Number(point.tension.toFixed(2))}`, cursor: "ew-resize" });
                line(`stem:${point.id}`, at, handle, "input", .22);
                marks.push({ type: "polyline", id: `handle:${point.id}`, points: [
                    { x: handle.x, y: handle.y - 3.4 }, { x: handle.x + 3.4, y: handle.y },
                    { x: handle.x, y: handle.y + 3.4 }, { x: handle.x - 3.4, y: handle.y },
                ], fill: true, ink: "input", opacity: .25 + point.tension * .75, target: id });
            }
            for (const i of indices) {
                const point = points[i];
                const at = marker(point), id = `point:${point.id}`;
                targets.push({ ...target(id, at, 8), wheel: true, cursorReturn: true, hint: pointStatus(point), shortcuts: pointShortcuts });
                if (selected.has(point.id))
                    marks.push({ type: "circle", id: `ring:${point.id}`, ...at, radius: 5.5, ink: "background", border: "selection" });
                marks.push({ type: "circle", id: `dot:${point.id}`, ...at, radius: selected.has(point.id) ? 3.2 : 2.6, ink: "input", target: id });
            }
        }

        const segment = selectedPoint();
        const tension = segment && address ? edits.drafts(address).find(({ id }) => id === segment.id)?.tension ?? segment.tension : undefined;
        if (stateChanged) commands.push({ type: "state", value: Object.keys(view).length ? readAutomationCurveView(view) : null });
        commands.push(...requests.takeCommands());
        const active = pointer ? edits.activePoint() : null;
        const sensitivity = active ? edits.sensitivityHint() : undefined;
        return {
            status: (address && edits.error(address)) || (editable ? undefined : "Read-only curve"),
            marks, targets, commands, itemEditing: editable || undefined,
            valueNavigation: !!address && !pointer && !marquee || undefined,
            cancelDrag: cancelDrag || undefined,
            drag: active || cursor ? { cursor, hint: active ? pointStatus(active) + (sensitivity ? ` · ${sensitivity}` : "") : undefined,
                shortcuts: sensitivity ? scrubSensitivityShortcut : undefined } : undefined,
            wakeAfterMs: continuation ? 0 : clock.nextDelay(),
            panel: editable ? { label: "Scale", rows: [
                choice("display", "Display", view.display ?? "curved", displayOptions),
                choice("ruler", "Value snapping", view.ruler ?? "auto", rulerOptions),
            ] } : undefined,
            contextMenu: editable && segment ? { label: "Automation segment", rows: [
                choice("segment", "Segment", segment.interpolation, segmentOptions),
                ...(segment.interpolation === "bezier" && tension !== undefined ? [[{ type: "text", id: "tension-label", text: "Tension" } as const,
                    { type: "knob", id: "tension", label: "Bézier tension", value: tension, ...automationTensionLimits, step: .05 } as const]] : []),
                [{ type: "action", id: "remove", label: "Delete point" }],
            ] } : undefined,
        };
    };
}

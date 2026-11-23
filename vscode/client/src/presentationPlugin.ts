import type { JsonValue, SerializedNodeEvent } from "./nodeEvents";
import { recordedTriggerBinsOperation, type ObservationWindowResult } from "./observations";
import type { NodeAddress } from "./graphScope";
import type { PresentationTime } from "./timelineSamples";
import { readPresentationSave, type SerializedPresentationState } from "./presentationStateJson";

export const presentationPluginVersion = 2;

export type PresentationManifest = {
    version: typeof presentationPluginVersion;
    id: string;
    title: string;
    definitionIds: string[];
    icon?: string;
    timeline?: true;
    timelineSnapping?: true;
    initialConfiguration?: string;
    observations?: Array<{ id: string; port: string; whenConnected?: true; completeOnly?: true; group?: string } & (
        { direction: "input" | "output"; operation?: "waveform"; points: number }
        | { direction: "output"; operation: typeof recordedTriggerBinsOperation })>;
};

export type PresentationPoint = { x: number; y: number };
export type PresentationCursor = PresentationPoint | { sample: string; yFraction: number };
export type PresentationModifiers = { shift: boolean; alt: boolean; control: boolean; meta: boolean };
export type PresentationObservation = {
    operation: "waveform" | typeof recordedTriggerBinsOperation;
    status: ObservationWindowResult["status"];
    error?: string;
    result?: ObservationWindowResult;
    spans: Array<{ begin: string; end: string; windowOffset: string; result: ObservationWindowResult }>;
};
export type PresentationSnapshot = {
    node: { address?: NodeAddress; definitionId: string; arguments: unknown[]; constructionDefinitionId?: string;
        ports: Array<{ name: string; direction: "input" | "output"; kind: "continuous" | "event";
            connected?: boolean; manualValue?: number }> };
    revision: number;
    clockMs?: number;
    mode: "editable" | "compact" | "mini" | "nano";
    editable: boolean;
    editsPending?: boolean;
    editError?: string;
    viewport: { width: number; height: number; time?: PresentationTime };
    observations: Record<string, PresentationObservation | null>;
    state: SerializedPresentationState;
};

export type PresentationGestureInput = { type: "gesture"; id: string;
    phase: "begin" | "move" | "modifiers" | "wheel" | "finish" | "cancel";
    point: PresentationPoint; delta?: PresentationPoint; wheel?: number; modifiers: PresentationModifiers;
    snapping?: { marks: string[]; playhead?: string } };

export type PresentationInput =
    | { type: "control"; id: string; phase: "preview" | "commit" | "finish"; value?: number | string }
    | PresentationGestureInput
    | { type: "context"; id: string; point: PresentationPoint }
    | { type: "dismissContext"; cancelled?: true }
    | { type: "items"; action: "delete" | "selectAll" }
    | { type: "valueView"; action: "pan"; delta: number }
    | { type: "valueView"; action: "zoom"; factor: number; y: number }
    | { type: "valueView"; action: "reset" }
    | { type: "editResult"; requestId: string; acceptedRevision?: number; error?: string }
    | { type: "wake" }
    | { type: "interrupt"; reason: "history" | "disconnected" };

export type PresentationControl =
    | { type: "text"; id: string; text: string }
    | { type: "knob"; id: string; label: string; value: number; min: number; max: number; defaultValue: number;
        integer?: boolean; step?: number; scrubSpeed?: number }
    | { type: "choice"; id: string; label: string; value: string; options: { value: string; label: string }[] }
    | { type: "action"; id: string; label: string; disabled?: boolean };

export type PresentationInk = "foreground" | "muted" | "input" | "output" | "selection" | "highlight" | "warning" | "background";
type PresentationStroke = { strokeWidth?: number; dashed?: true; fill?: true;
    fade?: { from: PresentationPoint; to: PresentationPoint } };
export type PresentationMark = { id: string; ink?: PresentationInk; opacity?: number; target?: string } & (
    | (PresentationStroke & ({ type: "polyline"; points: PresentationPoint[] }
        | { type: "packedPolyline"; coordinates: Float64Array }))
    | { type: "rect"; x: number; y: number; width: number; height: number; border?: PresentationInk }
    | { type: "circle"; x: number; y: number; radius: number; border?: PresentationInk }
    | { type: "text"; x: number; y: number; text: string });
export type PresentationTarget = { id: string; x: number; y: number; width: number; height: number;
    drag?: "relative" | "absolute"; button?: "primary" | "secondary"; wheel?: true; context?: true;
    cursorReturn?: true; hideCursor?: true; hint?: string; shortcuts?: string; cursor?: "default" | "ew-resize" };

export type PresentationCommand =
    | { type: "edit"; event: SerializedNodeEvent; gesture?: string; replacePending?: true; requestId?: string }
    | { type: "finish"; gesture: string; requestId?: string };

export function interruptedPresentationEditReplies(commands: readonly PresentationCommand[]): PresentationInput[] {
    return commands.flatMap((command): PresentationInput[] => command.requestId
        ? [{ type: "editResult", requestId: command.requestId, error: "Presentation edit interrupted" }] : []);
}

export type PresentationTimelineGrid = {
    numerator: string; denominator: string; strides: string[];
    lineStarts: number[]; minimumSpacing: number; opacity: number;
    ink?: PresentationInk; seek: boolean; labelMajor: boolean;
};

export type PresentationFrame = {
    status?: string;
    marks?: PresentationMark[];
    reuseDrawing?: true;
    targets?: PresentationTarget[];
    panel?: { label: string; rows: PresentationControl[][] };
    contextMenu?: { label: string; rows: PresentationControl[][] };
    itemEditing?: true;
    valueNavigation?: true;
    drag?: { hint?: string; shortcuts?: string; cursor?: PresentationCursor };
    cancelDrag?: true;
    wakeAfterMs?: number;
    secondsRulerInset?: number;
    timelineGrid?: PresentationTimelineGrid;
    commands?: PresentationCommand[];
    saveRequest?: SerializedPresentationState;
};
export type Present = (snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]) => PresentationFrame;
export type PresentationArtifact = { id: string; manifest: PresentationManifest; digest: string; bytes: string };

const record = (value: unknown): value is Record<string, any> => !!value && typeof value === "object" && !Array.isArray(value);
const text = (value: unknown, length = 160): value is string => typeof value === "string" && value.length <= length;
const identifier = (value: unknown): value is string => text(value) && value.length > 0 && /^[\w.:/-]+$/.test(value);
const finite = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value);
const list = <T>(value: unknown, max: number, item: (value: any) => value is T): value is T[] =>
    Array.isArray(value) && value.length <= max && value.every(item);
const point = (value: unknown): value is PresentationPoint => record(value) && finite(value.x) && finite(value.y);
function cursor(value: unknown): value is PresentationCursor {
    if (!record(value)) return false;
    if (!("sample" in value)) return point(value);
    return typeof value.sample === "string" && /^-?\d{1,19}$/.test(value.sample)
        && BigInt.asIntN(64, BigInt(value.sample)) === BigInt(value.sample)
        && finite(value.yFraction) && value.yFraction >= 0 && value.yFraction <= 1;
}
const flag = (value: unknown) => value === undefined || value === true;
const ink = (value: unknown): value is PresentationInk | undefined => value === undefined
    || ["foreground", "muted", "input", "output", "selection", "highlight", "warning", "background"].includes(value as string);

export function readPresentationManifest(value: unknown): PresentationManifest | undefined {
    if (!record(value) || value.version !== presentationPluginVersion || !identifier(value.id) || !text(value.title)
        || !list(value.definitionIds, 32, identifier) || !value.definitionIds.length
        || (value.icon !== undefined && !text(value.icon, 1024)) || !flag(value.timeline) || !flag(value.timelineSnapping)
        || (value.timelineSnapping && !value.timeline)
        || (value.initialConfiguration !== undefined
            && (!text(value.initialConfiguration, 8192) || !value.initialConfiguration.trim()))
        || (value.observations !== undefined && !list(value.observations, 8,
            (entry): entry is NonNullable<PresentationManifest["observations"]>[number] => record(entry)
                && identifier(entry.id) && identifier(entry.port) && (entry.direction === "input" || entry.direction === "output")
                && ((entry.operation === undefined || entry.operation === "waveform")
                    ? Number.isInteger(entry.points) && entry.points >= 2 && entry.points <= 1024
                    : entry.operation === recordedTriggerBinsOperation && entry.direction === "output" && entry.points === undefined)
                && flag(entry.whenConnected) && flag(entry.completeOnly)
                && (entry.group === undefined || identifier(entry.group))))) return;
    const manifest: PresentationManifest = value as PresentationManifest;
    const { version, id, title, definitionIds, icon, timeline, timelineSnapping, initialConfiguration, observations } = manifest;
    if (observations && new Set(observations.map(({ id }) => id)).size !== observations.length) return;
    return { version, id, title, definitionIds, icon, timeline, timelineSnapping, initialConfiguration, observations };
}

function control(value: unknown): value is PresentationControl {
    if (!record(value) || !identifier(value.id)) return false;
    switch (value.type) {
        case "text": return text(value.text, 1024);
        case "action": return text(value.label) && (value.disabled === undefined || typeof value.disabled === "boolean");
        case "knob": return text(value.label) && [value.value, value.min, value.max, value.defaultValue].every(finite)
            && (value.integer === undefined || typeof value.integer === "boolean")
            && (value.step === undefined || (finite(value.step) && value.step > 0))
            && (value.scrubSpeed === undefined || (finite(value.scrubSpeed) && value.scrubSpeed > 0));
        case "choice": return text(value.label) && text(value.value) && list(value.options, 64,
            (entry): entry is { value: string; label: string } => record(entry) && text(entry.value) && text(entry.label));
        default: return false;
    }
}

function area(value: Record<string, any>): boolean {
    return finite(value.x) && finite(value.y) && finite(value.width) && finite(value.height) && value.width >= 0 && value.height >= 0;
}

function stroke(value: Record<string, any>): boolean {
    return flag(value.dashed) && flag(value.fill)
        && (value.strokeWidth === undefined || (finite(value.strokeWidth) && value.strokeWidth >= 0 && value.strokeWidth <= 32))
        && (value.fade === undefined || (record(value.fade) && point(value.fade.from) && point(value.fade.to)));
}

function mark(value: unknown): value is PresentationMark {
    if (!record(value) || !identifier(value.id) || (value.opacity !== undefined && !finite(value.opacity))
        || !ink(value.ink) || (value.target !== undefined && !identifier(value.target))) return false;
    switch (value.type) {
        case "polyline": return list(value.points, 4096, point) && stroke(value);
        case "packedPolyline": return value.coordinates instanceof Float64Array && value.coordinates.length <= 8192
            && value.coordinates.length % 2 === 0 && value.coordinates.every(Number.isFinite) && stroke(value);
        case "rect": return area(value) && ink(value.border);
        case "circle": return finite(value.x) && finite(value.y) && finite(value.radius) && value.radius >= 0 && ink(value.border);
        case "text": return finite(value.x) && finite(value.y) && text(value.text, 1024);
        default: return false;
    }
}

function json(value: unknown, budget: { items: number; characters: number }, depth = 0): value is JsonValue {
    if (--budget.items < 0 || depth > 20) return false;
    if (value === null || typeof value === "boolean" || finite(value)) return true;
    if (typeof value === "string") return (budget.characters -= value.length) >= 0;
    if (Array.isArray(value)) return value.every((item) => json(item, budget, depth + 1));
    return record(value) && Object.prototype.toString.call(value) === "[object Object]"
        && Object.entries(value).every(([key, item]) => (budget.characters -= key.length) >= 0 && json(item, budget, depth + 1));
}

function command(value: unknown, budget: { items: number; characters: number }): value is PresentationCommand {
    if (!record(value)) return false;
    if (value.type === "finish") return identifier(value.gesture) && (value.requestId === undefined || identifier(value.requestId));
    return value.type === "edit" && (value.gesture === undefined || identifier(value.gesture))
        && (value.replacePending === undefined || (value.replacePending === true && identifier(value.gesture)))
        && (value.requestId === undefined || (identifier(value.requestId) && value.replacePending === undefined))
        && record(value.event) && record(value.event.eventType) && text(value.event.eventType.nominalId, 1024)
        && value.event.eventType.nominalId.length > 0 && text(value.event.eventType.definitionFingerprint, 1024)
        && json(value.event.payload, budget);
}

function panel(value: unknown): boolean {
    return value === undefined || (record(value) && text(value.label) && list(value.rows, 64,
        (row): row is PresentationControl[] => list(row, 12, control)));
}

function timelineGrid(value: unknown): value is PresentationTimelineGrid | undefined {
    if (value === undefined) return true;
    const positiveInteger = (entry: unknown): entry is string =>
        typeof entry === "string" && /^[1-9]\d{0,19}$/.test(entry) && BigInt(entry) < (1n << 64n);
    if (!record(value) || !positiveInteger(value.numerator) || !positiveInteger(value.denominator)
        || BigInt(value.numerator) < BigInt(value.denominator)
        || !list(value.strides, 32, positiveInteger) || value.strides.at(-1) !== "1"
        || !list(value.lineStarts, 32, (entry): entry is number => finite(entry) && entry >= 0 && entry <= 1)
        || value.lineStarts.length !== value.strides.length
        || !finite(value.minimumSpacing) || value.minimumSpacing < 1 || value.minimumSpacing > 128
        || !finite(value.opacity) || value.opacity < 0 || value.opacity > 1
        || typeof value.seek !== "boolean" || typeof value.labelMajor !== "boolean" || !ink(value.ink)) return false;
    return value.strides.every((stride, index, strides) => index === 0
        || BigInt(strides[index - 1]) % BigInt(stride) === 0n);
}

export function readPresentationFrame(value: unknown): PresentationFrame {
    const commandBudget = { items: 4096, characters: 65536 };
    if (!record(value) || (value.status !== undefined && !text(value.status, 512))
        || !flag(value.reuseDrawing) || (value.reuseDrawing && value.marks !== undefined)
        || (value.marks !== undefined && (!list(value.marks, 4096, mark)
        || value.marks.reduce((size, entry) => size + (entry.type === "polyline" ? entry.points.length
            : entry.type === "packedPolyline" ? entry.coordinates.length / 2 : 0), 0) > 16384))
        || (value.targets !== undefined && !list(value.targets, 1024,
            (entry): entry is PresentationTarget => record(entry) && identifier(entry.id) && area(entry)
                && (entry.drag === undefined || entry.drag === "relative" || entry.drag === "absolute")
                && (entry.button === undefined || entry.button === "primary" || entry.button === "secondary")
                && flag(entry.wheel) && flag(entry.context) && flag(entry.cursorReturn) && flag(entry.hideCursor)
                && (entry.hint === undefined || text(entry.hint, 512))
                && (entry.shortcuts === undefined || text(entry.shortcuts, 256))
                && (entry.cursor === undefined || entry.cursor === "default" || entry.cursor === "ew-resize")))
        || !panel(value.panel) || !panel(value.contextMenu) || !flag(value.itemEditing) || !flag(value.valueNavigation) || !flag(value.cancelDrag)
        || (value.wakeAfterMs !== undefined && (!finite(value.wakeAfterMs) || value.wakeAfterMs < 0 || value.wakeAfterMs > 10000))
        || (value.secondsRulerInset !== undefined && (!finite(value.secondsRulerInset) || value.secondsRulerInset < 0))
        || (value.drag !== undefined && (!record(value.drag) || (value.drag.cursor !== undefined && !cursor(value.drag.cursor))
            || (value.drag.hint !== undefined && !text(value.drag.hint, 512))
            || (value.drag.shortcuts !== undefined && !text(value.drag.shortcuts, 256))))
        || (value.commands !== undefined && !list(value.commands, 32,
            (entry): entry is PresentationCommand => command(entry, commandBudget)))
        || !timelineGrid(value.timelineGrid) || (value.reuseDrawing && value.timelineGrid))
        throw new Error("Invalid presentation frame");
    const { status, marks, reuseDrawing, targets, panel: settings, contextMenu, itemEditing, valueNavigation, drag, cancelDrag,
        wakeAfterMs, secondsRulerInset, commands, saveRequest, timelineGrid: grid } = value as PresentationFrame;
    for (const entries of [marks, targets, settings?.rows.flat(), contextMenu?.rows.flat()]) {
        if (entries && new Set(entries.map(({ id }) => id)).size !== entries.length) throw new Error("Duplicate presentation item ID");
    }
    return { status, marks, ...(reuseDrawing && { reuseDrawing }), targets, panel: settings, contextMenu,
        itemEditing, valueNavigation, drag, cancelDrag, wakeAfterMs, secondsRulerInset, commands, timelineGrid: grid,
        saveRequest: saveRequest === undefined ? undefined : readPresentationSave(saveRequest) };
}

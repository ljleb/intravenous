import type { JsonValue, SerializedNodeEvent } from "./nodeEvents";
import type { ObservationWindowResult } from "./observations";

export const presentationPluginVersion = 1;

export type PresentationManifest = {
    version: typeof presentationPluginVersion;
    id: string;
    title: string;
    definitionIds: string[];
    icon?: string;
    timeline?: true;
    initialConfiguration?: string;
    observations?: Array<{ id: string; direction: "input" | "output"; port: string; points: number }>;
};

export type PresentationPoint = { x: number; y: number };
export type PresentationModifiers = { shift: boolean; alt: boolean; control: boolean; meta: boolean };
export type PresentationObservation = {
    status: ObservationWindowResult["status"];
    error?: string;
    result: ObservationWindowResult;
    spans: Array<{ begin: string; end: string; result: ObservationWindowResult }>;
};
export type PresentationSnapshot = {
    node: { definitionId: string; arguments: unknown[]; ports: Array<{ name: string; direction: "input" | "output"; kind: "continuous" | "event" }> };
    revision: number;
    mode: "editable" | "compact" | "mini" | "nano";
    editable: boolean;
    editsPending?: boolean;
    editError?: string;
    viewport: { width: number; height: number; time?: { beginSeconds: number; spanSeconds: number; sampleRate?: number } };
    observations: Record<string, PresentationObservation | null>;
    state: JsonValue;
};

export type PresentationInput =
    | { type: "control"; id: string; phase: "preview" | "commit" | "finish"; value?: number | string }
    | { type: "gesture"; id: string; phase: "begin" | "move" | "modifiers" | "finish" | "cancel";
        point: PresentationPoint; delta?: PresentationPoint; modifiers: PresentationModifiers };

export type PresentationControl =
    | { type: "text"; id: string; text: string }
    | { type: "knob"; id: string; label: string; value: number; min: number; max: number; defaultValue: number; integer?: boolean }
    | { type: "choice"; id: string; label: string; value: string; options: { value: string; label: string }[] }
    | { type: "action"; id: string; label: string; disabled?: boolean };

export type PresentationInk = "foreground" | "muted" | "input" | "output" | "selection";
export type PresentationMark = { id: string; ink?: PresentationInk; opacity?: number } & (
    | { type: "polyline"; points: PresentationPoint[] }
    | { type: "rect"; x: number; y: number; width: number; height: number }
    | { type: "text"; x: number; y: number; text: string });
export type PresentationTarget = { id: string; x: number; y: number; width: number; height: number };

export type PresentationCommand =
    | { type: "edit"; event: SerializedNodeEvent; gesture?: string; replacePending?: true }
    | { type: "finish"; gesture: string }
    | { type: "state"; value: JsonValue };

export type PresentationFrame = {
    status?: string;
    marks?: PresentationMark[];
    targets?: PresentationTarget[];
    panel?: { label: string; rows: PresentationControl[][] };
    seekMarks?: string[];
    commands?: PresentationCommand[];
};
export type PresentationSeekWindow = { first: string; length: string; sampleRate: number; width: number };
export type ReadPresentationSeekMarks = (window: PresentationSeekWindow) => readonly string[];
export type Present = ((snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]) => PresentationFrame) & {
    openSeekReader?: () => ReadPresentationSeekMarks | undefined;
};
export type PresentationScript = { manifest: PresentationManifest; source: string };
export type PresentationProgram = PresentationScript | { manifest: PresentationManifest; create: () => Present };

const record = (value: unknown): value is Record<string, any> => !!value && typeof value === "object" && !Array.isArray(value);
const text = (value: unknown, length = 160): value is string => typeof value === "string" && value.length <= length;
const identifier = (value: unknown): value is string => text(value) && value.length > 0 && /^[\w.:/-]+$/.test(value);
const finite = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value);
const list = <T>(value: unknown, max: number, item: (value: any) => value is T): value is T[] =>
    Array.isArray(value) && value.length <= max && value.every(item);

export function readPresentationManifest(value: unknown): PresentationManifest | undefined {
    if (!record(value) || value.version !== presentationPluginVersion || !identifier(value.id) || !text(value.title)
        || !list(value.definitionIds, 32, identifier) || !value.definitionIds.length
        || (value.icon !== undefined && !text(value.icon, 1024)) || (value.timeline !== undefined && value.timeline !== true)
        || (value.initialConfiguration !== undefined
            && (!text(value.initialConfiguration, 8192) || !value.initialConfiguration.trim()))
        || (value.observations !== undefined && !list(value.observations, 8,
            (entry): entry is NonNullable<PresentationManifest["observations"]>[number] => record(entry)
                && identifier(entry.id) && identifier(entry.port) && (entry.direction === "input" || entry.direction === "output")
                && Number.isInteger(entry.points) && entry.points >= 2 && entry.points <= 1024))) return;
    const manifest: PresentationManifest = value as PresentationManifest;
    const { version, id, title, definitionIds, icon, timeline, initialConfiguration, observations } = manifest;
    if (observations && new Set(observations.map(({ id }) => id)).size !== observations.length) return;
    return { version, id, title, definitionIds, icon, timeline, initialConfiguration, observations };
}

function control(value: unknown): value is PresentationControl {
    if (!record(value) || !identifier(value.id)) return false;
    switch (value.type) {
        case "text": return text(value.text, 1024);
        case "action": return text(value.label) && (value.disabled === undefined || typeof value.disabled === "boolean");
        case "knob": return text(value.label) && [value.value, value.min, value.max, value.defaultValue].every(finite)
            && (value.integer === undefined || typeof value.integer === "boolean");
        case "choice": return text(value.label) && text(value.value) && list(value.options, 64,
            (entry): entry is { value: string; label: string } => record(entry) && text(entry.value) && text(entry.label));
        default: return false;
    }
}

function area(value: Record<string, any>): boolean {
    return finite(value.x) && finite(value.y) && finite(value.width) && finite(value.height) && value.width >= 0 && value.height >= 0;
}

function mark(value: unknown): value is PresentationMark {
    if (!record(value) || !identifier(value.id) || (value.opacity !== undefined && !finite(value.opacity))
        || (value.ink !== undefined && !["foreground", "muted", "input", "output", "selection"].includes(value.ink))) return false;
    switch (value.type) {
        case "polyline": return list(value.points, 4096, (point): point is PresentationPoint =>
            record(point) && finite(point.x) && finite(point.y));
        case "rect": return area(value);
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
    if (value.type === "finish") return identifier(value.gesture);
    if (value.type === "state") return json(value.value, budget);
    return value.type === "edit" && (value.gesture === undefined || identifier(value.gesture))
        && (value.replacePending === undefined || (value.replacePending === true && identifier(value.gesture)))
        && record(value.event) && record(value.event.eventType) && text(value.event.eventType.nominalId, 1024)
        && value.event.eventType.nominalId.length > 0 && text(value.event.eventType.definitionFingerprint, 1024)
        && json(value.event.payload, budget);
}

export function readPresentationFrame(value: unknown): PresentationFrame {
    const commandBudget = { items: 4096, characters: 65536 };
    if (!record(value) || (value.status !== undefined && !text(value.status, 512))
        || (value.marks !== undefined && (!list(value.marks, 4096, mark)
        || value.marks.reduce((size, entry) => size + (entry.type === "polyline" ? entry.points.length : 0), 0) > 16384))
        || (value.targets !== undefined && !list(value.targets, 1024,
            (entry): entry is PresentationTarget => record(entry) && identifier(entry.id) && area(entry)))
        || (value.panel !== undefined && (!record(value.panel) || !text(value.panel.label) || !list(value.panel.rows, 64,
            (row): row is PresentationControl[] => list(row, 12, control))))
        || (value.commands !== undefined && !list(value.commands, 32,
            (entry): entry is PresentationCommand => command(entry, commandBudget)))
        || (value.seekMarks !== undefined && !list(value.seekMarks, 4096,
            (entry): entry is string => typeof entry === "string" && /^-?\d{1,19}$/.test(entry) && BigInt.asIntN(64, BigInt(entry)) === BigInt(entry))))
        throw new Error("Invalid presentation frame");
    const { status, marks, targets, panel, commands, seekMarks } = value as PresentationFrame;
    for (const entries of [marks, targets, panel?.rows.flat()]) {
        if (entries && new Set(entries.map(({ id }) => id)).size !== entries.length) throw new Error("Duplicate presentation item ID");
    }
    return { status, marks, targets, panel, commands, seekMarks };
}

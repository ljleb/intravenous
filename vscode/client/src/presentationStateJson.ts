import type { JsonValue } from "./nodeEvents";

export type PresentationStateValue = { [name: string]: JsonValue } | null;
export type SerializedPresentationState = string | null;
export const maxPresentationStateBytes = 128 * 1024;
const encoder = new TextEncoder();

export function parsePresentationState(state: SerializedPresentationState): PresentationStateValue {
    return state === null ? null : JSON.parse(state);
}

export function readPresentationSave(value: unknown): SerializedPresentationState {
    if (value === null) return null;
    if (typeof value !== "string" || value.length > maxPresentationStateBytes
        || encoder.encode(value).length > maxPresentationStateBytes) throw new Error("Invalid presentation save payload");
    const state: unknown = JSON.parse(value);
    if (state === null) return null;
    if (typeof state !== "object" || Array.isArray(state)) throw new Error("Invalid presentation save payload");
    const pending: unknown[] = [state];
    while (pending.length) {
        const entry = pending.pop();
        if (typeof entry === "number" && !Number.isFinite(entry)) throw new Error("Invalid presentation save number");
        if (entry && typeof entry === "object") for (const item of Object.values(entry)) pending.push(item);
    }
    return value;
}

export function serializePresentationState(state: PresentationStateValue): SerializedPresentationState {
    return state === null ? null : json(state);
}

function json(value: JsonValue): string {
    if (Object.is(value, -0)) return "-0.0";
    if (Array.isArray(value)) return `[${value.map(json).join(",")}]`;
    if (value !== null && typeof value === "object") {
        const members = Object.entries(value).map(([key, member]) => `${JSON.stringify(key)}:${json(member)}`);
        return `{${members.join(",")}}`;
    }
    return JSON.stringify(value);
}

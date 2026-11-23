import type { JsonValue } from "./nodeEvents";

export type PresentationStateValue = { [name: string]: JsonValue } | null;
export type SerializedPresentationState = string | null;
export const maxPresentationStateBytes = 128 * 1024;
const encoder = new TextEncoder();
const unpairedSurrogate = /[\uD800-\uDFFF]/u;

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
    validateSaveTokens(value);
    return value;
}

function validateSaveTokens(json: string): void {
    // The document has passed syntax validation. Read the original tokens so
    // a later property cannot hide a value replaced by JSON.parse.
    for (let offset = 0; offset < json.length;) {
        const start = offset++;
        if (json[start] === '"') {
            while (json[offset] !== '"') offset += json[offset] === "\\" ? 2 : 1;
            const text: string = JSON.parse(json.slice(start, ++offset));
            if (unpairedSurrogate.test(text)) throw new Error("Invalid Unicode in presentation save");
        } else if (json[start] === "-" || (json[start] >= "0" && json[start] <= "9")) {
            while (offset < json.length && /[0-9.eE+-]/.test(json[offset])) ++offset;
            if (!Number.isFinite(Number(json.slice(start, offset)))) throw new Error("Invalid presentation save number");
        }
    }
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

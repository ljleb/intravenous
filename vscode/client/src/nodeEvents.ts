import { addressKey, type NodeAddress } from "./graphScope";
export type { NodeAddress } from "./graphScope";

export type JsonValue = null | boolean | number | string | JsonValue[] | { [name: string]: JsonValue };
export type NodeEventTypeId = { nominalId: string; definitionFingerprint: string };
export type NodeEventValueSchema = {
    kind: "unsupported" | "boolean" | "signedInteger" | "unsignedInteger" | "floatingPoint"
        | "string" | "convertedScalar" | "enumeration" | "aggregate" | "sequence" | "nodeAddress";
    reason?: string;
    bitWidth?: number;
    signed?: boolean;
    fields?: Array<{ name: string; value: NodeEventValueSchema }>;
    enumerators?: Array<{ name: string; value: string }>;
    element?: NodeEventValueSchema;
    fixedElementCount?: number;
};
export type NodeEventSchema = NodeEventTypeId & { displayName: string; payload: NodeEventValueSchema };
export type NodeEventProtocol = {
    formatVersion: number;
    projectGraphGeneration?: number;
    definitionsGeneration?: number;
    eventTypes: NodeEventSchema[];
    definitions: Array<{ definitionId: string; supportedEventTypes: NodeEventTypeId[] }>;
    receivers?: Array<{ address: NodeAddress; supportedEventTypes: NodeEventTypeId[] }>;
};
export type SerializedNodeEvent = { eventType: NodeEventTypeId; payload: JsonValue };

declare const eventBrand: unique symbol;
// Only factories create event values. The payload type stays visible to TypeScript
// without exposing transport arguments to presentation authors.
export interface NodeEvent<Payload = unknown> { readonly [eventBrand]: Payload }
const events = new WeakMap<object, SerializedNodeEvent>();
const sameType = (left: NodeEventTypeId, right: NodeEventTypeId) =>
    left.nominalId === right.nominalId && left.definitionFingerprint === right.definitionFingerprint;

export function encodeNodeEventValue(schema: NodeEventValueSchema, value: unknown, path = "payload"): JsonValue {
    const invalid = (reason: string): never => { throw new Error(`${path}: ${reason}`); };
    switch (schema.kind) {
        case "boolean": return typeof value === "boolean" ? value : invalid("expected a Boolean");
        case "string": return typeof value === "string" ? value : invalid("expected a string");
        case "signedInteger": case "unsignedInteger": {
            if (typeof value !== "number" || !Number.isSafeInteger(value)) return invalid("expected an exactly representable integer");
            const bits = schema.bitWidth;
            if (!bits || bits < 1 || bits > 64) return invalid("unsupported integer width");
            const unsigned = schema.kind === "unsignedInteger";
            if (value < (unsigned ? 0 : -(2 ** (bits - 1))) || value >= 2 ** (bits - (unsigned ? 0 : 1)))
                return invalid("integer is outside its declared range");
            return value;
        }
        case "floatingPoint":
            if (typeof value !== "number" || !Number.isFinite(value)
                || (schema.bitWidth === 32 && !Number.isFinite(Math.fround(value))))
                return invalid("expected a finite value in the declared floating-point range");
            return value;
        case "convertedScalar":
            if (typeof value === "boolean" || typeof value === "string") return value;
            return typeof value === "number" && Number.isFinite(value) ? value : invalid("expected a finite scalar");
        case "enumeration":
            return typeof value === "string" && schema.enumerators?.some((entry) => entry.name === value)
                ? value : invalid("unknown enumerator name");
        case "aggregate": {
            if (!value || typeof value !== "object" || Array.isArray(value)) return invalid("expected an object");
            const fields = schema.fields ?? [];
            if (Object.keys(value).some((key) => !fields.some((field) => field.name === key))) return invalid("unknown field");
            return Object.fromEntries(fields.map((field) => [field.name,
                encodeNodeEventValue(field.value, (value as Record<string, unknown>)[field.name], `${path}.${field.name}`)]));
        }
        case "sequence":
            if (!Array.isArray(value) || !schema.element) return invalid("expected a supported sequence");
            if (schema.fixedElementCount && value.length !== schema.fixedElementCount) return invalid("wrong fixed sequence length");
            return value.map((element, index) => encodeNodeEventValue(schema.element!, element, `${path}[${index}]`));
        case "nodeAddress": {
            if (!value || typeof value !== "object" || typeof (value as NodeAddress).instanceId !== "string")
                return invalid("expected a node address");
            const address = value as NodeAddress;
            if (address.path !== undefined && !Array.isArray(address.path)) return invalid("invalid node selector path");
            // Selector validation and address resolution belong to the backend.
            return JSON.parse(JSON.stringify({ instanceId: address.instanceId, path: address.path ?? [] })) as JsonValue;
        }
        default: return invalid(schema.reason ?? "unsupported event payload type");
    }
}

export function defineNodeEvent<Payload>(eventType: NodeEventTypeId, schema: NodeEventValueSchema): (payload: Payload) => NodeEvent<Payload> {
    // Capture independent metadata so a caller cannot mutate a factory's identity.
    const identity = { ...eventType };
    const payloadSchema = structuredClone(schema);
    return (payload) => {
        const event = Object.freeze({}) as NodeEvent<Payload>;
        events.set(event, { eventType: identity, payload: encodeNodeEventValue(payloadSchema, payload) });
        return event;
    };
}

export function serializeNodeEvent(event: NodeEvent): SerializedNodeEvent {
    const value = event && events.get(event);
    if (!value) throw new Error("node.send() requires a typed event created by a node-event factory");
    return structuredClone(value);
}

export function validateNodeEventTarget(protocol: NodeEventProtocol, target: NodeAddress, event: SerializedNodeEvent): void {
    if (protocol.formatVersion !== 0) throw new Error("Unsupported node-event protocol version");
    const schema = protocol.eventTypes.find((entry) => sameType(entry, event.eventType));
    if (!schema) throw new Error("The node-event definition changed or is unavailable; regenerate its frontend binding");
    const receiver = protocol.receivers?.find((entry) => addressKey(entry.address) === addressKey(target));
    if (!receiver?.supportedEventTypes.some((entry) => sameType(entry, event.eventType)))
        throw new Error("The target node no longer supports this exact event type");
    encodeNodeEventValue(schema.payload, event.payload);
}

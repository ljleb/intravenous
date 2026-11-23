import type { JsonValue, NodeEventTypeId, NodeEventValueSchema, SerializedNodeEvent } from "./nodeEvents";

const maximumSchemaBytes = 64 * 1024;
const maximumEventItems = 4096;
const maximumStringBytes = 64 * 1024;

type Budget = { items: number; bytes: number };
type Reader = { view: DataView; check: (at: number, size: number, alignment: number) => void };
type Layout = {
    size: number;
    alignment: number;
    read(reader: Reader, at: number, budget: Budget): JsonValue;
};
type Binding = { eventType: NodeEventTypeId; layout: Layout };
const decoder = new TextDecoder("utf-8", { fatal: true });
const align = (size: number, alignment: number) => Math.ceil(size / alignment) * alignment;
const invalid = (reason: string): never => { throw new Error(`Invalid presentation event: ${reason}`); };

function consume(budget: Budget) {
    if (--budget.items < 0) invalid("payload exceeds its item budget");
}

function fixed(size: number, alignment: number, read: Layout["read"]): Layout {
    return { size, alignment, read(reader, at, budget) { consume(budget); return read(reader, at, budget); } };
}

function string({ view, check }: Reader, at: number, budget: Budget): string {
    const start = view.getUint32(at, true), length = view.getUint32(at + 4, true);
    if ((budget.bytes -= length) < 0) invalid("payload exceeds its string budget");
    if (!length) return "";
    check(start, length, 1);
    return decoder.decode(new Uint8Array(view.buffer, start, length));
}

function integerBytes(width: number | undefined): number {
    if (width === undefined || ![8, 16, 32, 64].includes(width)) return invalid("unsupported integer width");
    return width / 8;
}

function integer(view: DataView, at: number, bytes: number, signed: boolean): bigint {
    switch (bytes) {
        case 1: return BigInt(signed ? view.getInt8(at) : view.getUint8(at));
        case 2: return BigInt(signed ? view.getInt16(at, true) : view.getUint16(at, true));
        case 4: return BigInt(signed ? view.getInt32(at, true) : view.getUint32(at, true));
        default: return signed ? view.getBigInt64(at, true) : view.getBigUint64(at, true);
    }
}

function boolean(value: number): boolean {
    return value === 0 ? false : value === 1 ? true : invalid("invalid Boolean");
}

function finite(value: number): number {
    return Number.isFinite(value) ? value : invalid("nonfinite number");
}

function compileLayout(schema: NodeEventValueSchema, budget: Budget, depth = 0): Layout {
    consume(budget);
    if (!schema || typeof schema !== "object" || depth > 16) return invalid("invalid payload schema");
    switch (schema.kind) {
        case "boolean":
            return fixed(1, 1, ({ view }, at) => boolean(view.getUint8(at)));
        case "floatingPoint": {
            const width = schema.bitWidth;
            if (width !== 32 && width !== 64) return invalid("unsupported floating-point width");
            return fixed(width / 8, width / 8, ({ view }, at) =>
                finite(width === 32 ? view.getFloat32(at, true) : view.getFloat64(at, true)));
        }
        case "signedInteger": case "unsignedInteger": {
            const bytes = integerBytes(schema.bitWidth);
            const signed = schema.kind === "signedInteger";
            return fixed(bytes, bytes, ({ view }, at) => {
                const value = Number(integer(view, at, bytes, signed));
                return Number.isSafeInteger(value) ? value : invalid("integer cannot be sent exactly over node-event RPC");
            });
        }
        case "enumeration": {
            const bytes = integerBytes(schema.bitWidth);
            const names = new Map<bigint, string>();
            if (!Array.isArray(schema.enumerators) || schema.enumerators.length > 1024) return invalid("invalid enumerators");
            for (const entry of schema.enumerators) {
                consume(budget);
                if (typeof entry?.name !== "string" || typeof entry.value !== "string" || !/^-?\d{1,20}$/.test(entry.value))
                    return invalid("invalid enumerator");
                const value = BigInt(entry.value);
                if (!names.has(value)) names.set(value, entry.name);
            }
            const signed = schema.signed === true;
            return fixed(bytes, bytes, ({ view }, at) =>
                names.get(integer(view, at, bytes, signed)) ?? invalid("unknown enum value"));
        }
        case "string": return fixed(8, 4, string);
        case "convertedScalar":
            return fixed(24, 8, (reader, at, budget) => {
                const { view } = reader;
                switch (view.getUint32(at, true)) {
                    case 0: return boolean(view.getFloat64(at + 8, true));
                    case 1: return finite(view.getFloat64(at + 8, true));
                    case 2: return string(reader, at + 16, budget);
                    default: return invalid("unknown converted scalar kind");
                }
            });
        case "sequence": {
            if (!schema.element) return invalid("sequence has no element layout");
            const element = compileLayout(schema.element, budget, depth + 1);
            const exactCount = schema.fixedElementCount;
            if (exactCount !== undefined && (!Number.isSafeInteger(exactCount) || exactCount < 0
                || exactCount > maximumEventItems)) return invalid("invalid fixed sequence length");
            return fixed(8, 4, (reader, at, budget) => {
                const start = reader.view.getUint32(at, true), count = reader.view.getUint32(at + 4, true);
                if (count > budget.items || (exactCount !== undefined && count !== exactCount)) return invalid("invalid sequence length");
                if (!count) return [];
                reader.check(start, element.size * count, element.alignment);
                return Array.from({ length: count }, (_, index) => element.read(reader, start + index * element.size, budget));
            });
        }
        case "aggregate": {
            if (!Array.isArray(schema.fields) || schema.fields.length > 256) return invalid("invalid fields");
            let size = 0, alignment = 1;
            const names = new Set<string>();
            const fields = schema.fields.map((field) => {
                const name = field?.name;
                if (typeof name !== "string" || !/^[A-Za-z_]\w{0,159}$/.test(name) || names.has(name))
                    return invalid("invalid or duplicate field name");
                names.add(name);
                const layout = compileLayout(field.value, budget, depth + 1);
                alignment = Math.max(alignment, layout.alignment);
                const offset = align(size, layout.alignment);
                size = offset + layout.size;
                return { name, layout, offset };
            });
            return fixed(align(Math.max(size, 1), alignment), alignment, (reader, at, budget) =>
                Object.fromEntries(fields.map(({ name, layout, offset }) => [name, layout.read(reader, at + offset, budget)])));
        }
        default: return invalid("unsupported value schema");
    }
}

export function readWasmEventBindings(module: WebAssembly.Module): readonly Binding[] {
    const sections = WebAssembly.Module.customSections(module, "iv.presentation.events");
    if (!sections.length) return [];
    if (sections.length !== 1 || sections[0].byteLength > maximumSchemaBytes) return invalid("invalid schema section");
    const entries: unknown = JSON.parse(decoder.decode(sections[0]));
    if (!Array.isArray(entries) || entries.length > 64) return invalid("too many bindings");
    const budget = { items: 4096, bytes: maximumSchemaBytes };
    return entries.map((entry) => {
        if (typeof entry?.nominalId !== "string" || !entry.nominalId || entry.nominalId.length > 1024
            || typeof entry.definitionFingerprint !== "string" || !entry.definitionFingerprint
            || entry.definitionFingerprint.length > 1024 || entry.payload?.kind !== "aggregate")
            return invalid("invalid binding schema");
        return {
            eventType: { nominalId: entry.nominalId, definitionFingerprint: entry.definitionFingerprint },
            layout: compileLayout(entry.payload, budget),
        };
    });
}

export function wasmEventReader(bindings: readonly Binding[], reader: Reader) {
    const budget = { items: maximumEventItems, bytes: maximumStringBytes };
    return (index: number, address: number): SerializedNodeEvent => {
        const binding = bindings[index];
        if (!binding || !address) return invalid("unknown binding or missing payload");
        const { layout, eventType } = binding;
        reader.check(address, layout.size, layout.alignment);
        return { eventType, payload: layout.read(reader, address, budget) };
    };
}

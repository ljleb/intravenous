import type { JsonValue, NodeEventValueSchema } from "./nodeEvents";

const maximumEventItems = 4096;

export type Budget = { items: number; bytes: number };
export type Reader = { view: DataView; check: (at: number, size: number, alignment: number) => void };
type Writer = {
    view: DataView;
    reserve(size: number, alignment: number): number;
    pointer(at: number, offset: number): void;
};
export type Layout = {
    size: number;
    alignment: number;
    read(reader: Reader, at: number, budget: Budget): JsonValue;
    write(writer: Writer, at: number, value: unknown, budget: Budget): void;
};
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
export const align = (size: number, alignment: number) => Math.ceil(size / alignment) * alignment;
const invalid = (reason: string): never => { throw new Error(`Invalid presentation value: ${reason}`); };

export function readWasmBindingSection(module: WebAssembly.Module, name: string): unknown[] {
    const sections = WebAssembly.Module.customSections(module, name);
    if (!sections.length) return [];
    if (sections.length !== 1 || sections[0].byteLength > 64 * 1024) return invalid(`invalid ${name} schema section`);
    const entries: unknown = JSON.parse(decoder.decode(sections[0]));
    if (!Array.isArray(entries) || entries.length > 64) return invalid(`too many ${name} bindings`);
    return entries;
}

function consume(budget: Budget) {
    if (--budget.items < 0) invalid("payload exceeds its item budget");
}

function fixed(size: number, alignment: number, read: Layout["read"], write: Layout["write"]): Layout {
    return {
        size, alignment,
        read(reader, at, budget) { consume(budget); return read(reader, at, budget); },
        write(writer, at, value, budget) { consume(budget); write(writer, at, value, budget); },
    };
}

function string({ view, check }: Reader, at: number, budget: Budget): string {
    const start = view.getUint32(at, true), length = view.getUint32(at + 4, true);
    if ((budget.bytes -= length) < 0) invalid("payload exceeds its string budget");
    if (!length) return "";
    check(start, length, 1);
    return decoder.decode(new Uint8Array(view.buffer, start, length));
}

function writeString(writer: Writer, at: number, value: unknown, budget: Budget): void {
    if (typeof value !== "string") return invalid("expected a string");
    if (value.length > budget.bytes) return invalid("payload exceeds its string budget");
    const bytes = encoder.encode(value);
    if ((budget.bytes -= bytes.length) < 0) return invalid("payload exceeds its string budget");
    const start = bytes.length ? writer.reserve(bytes.length, 1) : 0;
    if (bytes.length) new Uint8Array(writer.view.buffer, start, bytes.length).set(bytes);
    writer.pointer(at, start);
    writer.view.setUint32(at + 4, bytes.length, true);
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

function writeInteger(view: DataView, at: number, bytes: number, signed: boolean, value: bigint): void {
    const bits = BigInt(bytes * 8);
    const limit = 1n << (signed ? bits - 1n : bits);
    if (value < (signed ? -limit : 0n) || value >= limit) return invalid("integer is outside its declared range");
    switch (bytes) {
        case 1: view.setUint8(at, Number(value)); break;
        case 2: view.setUint16(at, Number(value), true); break;
        case 4: view.setUint32(at, Number(value), true); break;
        default: view.setBigUint64(at, value, true); break;
    }
}

function integerValue(value: unknown): bigint {
    if (typeof value === "number" && Number.isSafeInteger(value)) return BigInt(value);
    if (typeof value === "string" && /^-?\d{1,20}$/.test(value)) return BigInt(value);
    return invalid("expected an exact integer");
}

function boolean(value: number): boolean {
    return value === 0 ? false : value === 1 ? true : invalid("invalid Boolean");
}

function finite(value: number): number {
    return Number.isFinite(value) ? value : invalid("nonfinite number");
}

export function compileLayout(schema: NodeEventValueSchema, budget: Budget, depth = 0): Layout {
    consume(budget);
    if (!schema || typeof schema !== "object" || depth > 16) return invalid("invalid payload schema");
    switch (schema.kind) {
        case "boolean":
            return fixed(1, 1, ({ view }, at) => boolean(view.getUint8(at)), ({ view }, at, value) => {
                if (typeof value !== "boolean") return invalid("expected a Boolean");
                view.setUint8(at, +value);
            });
        case "floatingPoint": {
            const width = schema.bitWidth;
            if (width !== 32 && width !== 64) return invalid("unsupported floating-point width");
            return fixed(width / 8, width / 8,
                ({ view }, at) => finite(width === 32 ? view.getFloat32(at, true) : view.getFloat64(at, true)),
                ({ view }, at, value) => {
                    if (typeof value !== "number") return invalid("expected a number");
                    if (width === 32) view.setFloat32(at, finite(Math.fround(value)), true);
                    else view.setFloat64(at, finite(value), true);
                });
        }
        case "signedInteger": case "unsignedInteger": {
            const bytes = integerBytes(schema.bitWidth);
            const signed = schema.kind === "signedInteger";
            return fixed(bytes, bytes, ({ view }, at) => {
                const value = Number(integer(view, at, bytes, signed));
                return Number.isSafeInteger(value) ? value : invalid("integer cannot be sent exactly over node-event RPC");
            }, ({ view }, at, value) => writeInteger(view, at, bytes, signed, integerValue(value)));
        }
        case "enumeration": {
            const bytes = integerBytes(schema.bitWidth);
            const names = new Map<bigint, string>();
            const values = new Map<string, bigint>();
            if (!Array.isArray(schema.enumerators) || schema.enumerators.length > 1024) return invalid("invalid enumerators");
            for (const entry of schema.enumerators) {
                consume(budget);
                if (typeof entry?.name !== "string" || typeof entry.value !== "string" || !/^-?\d{1,20}$/.test(entry.value))
                    return invalid("invalid enumerator");
                const value = BigInt(entry.value);
                if (!names.has(value)) names.set(value, entry.name);
                values.set(entry.name, value);
            }
            const signed = schema.signed === true;
            return fixed(bytes, bytes,
                ({ view }, at) => names.get(integer(view, at, bytes, signed)) ?? invalid("unknown enum value"),
                ({ view }, at, value) => {
                    const number = typeof value === "string" ? values.get(value) : undefined;
                    if (number === undefined) return invalid("unknown enumerator name");
                    writeInteger(view, at, bytes, signed, number);
                });
        }
        case "string": return fixed(8, 4, string, writeString);
        case "convertedScalar":
            return fixed(24, 8, (reader, at, budget) => {
                const { view } = reader;
                switch (view.getUint32(at, true)) {
                    case 0: return boolean(view.getFloat64(at + 8, true));
                    case 1: return finite(view.getFloat64(at + 8, true));
                    case 2: return string(reader, at + 16, budget);
                    default: return invalid("unknown converted scalar kind");
                }
            }, (writer, at, value, budget) => {
                if (typeof value === "boolean") {
                    writer.view.setUint32(at, 0, true);
                    writer.view.setFloat64(at + 8, +value, true);
                } else if (typeof value === "number") {
                    writer.view.setUint32(at, 1, true);
                    writer.view.setFloat64(at + 8, finite(value), true);
                } else {
                    writer.view.setUint32(at, 2, true);
                    writeString(writer, at + 16, value, budget);
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
            }, (writer, at, value, budget) => {
                if (!Array.isArray(value) || value.length > budget.items
                    || (exactCount !== undefined && value.length !== exactCount)) return invalid("invalid sequence length");
                const start = value.length ? writer.reserve(element.size * value.length, element.alignment) : 0;
                writer.pointer(at, start);
                writer.view.setUint32(at + 4, value.length, true);
                value.forEach((item, index) => element.write(writer, start + index * element.size, item, budget));
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
            return fixed(align(Math.max(size, 1), alignment), alignment,
                (reader, at, budget) => Object.fromEntries(fields.map(({ name, layout, offset }) =>
                    [name, layout.read(reader, at + offset, budget)])),
                (writer, at, value, budget) => {
                    if (!value || typeof value !== "object" || Array.isArray(value)) return invalid("expected a record");
                    if (Object.keys(value).some((key) => !names.has(key))) return invalid("unknown field");
                    const record = value as Record<string, unknown>;
                    for (const { name, layout, offset } of fields)
                        layout.write(writer, at + offset, record[name], budget);
                });
        }
        default: return invalid("unsupported value schema");
    }
}

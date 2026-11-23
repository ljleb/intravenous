import type { NodeEventValueSchema } from "./nodeEvents";
import { align, compileLayout, readWasmBindingSection, WasmPresentationValueError, type Layout } from "./wasmPresentationValues";

export type WasmConstruction = { definitionId: string; arguments: readonly unknown[] };
type Binding = { definitionId: string; index: number; layout: Layout };

export function readWasmArgumentBindings(module: WebAssembly.Module): readonly Binding[] {
    const budget = { items: 4096, bytes: 0 };
    return readWasmBindingSection(module, "iv.presentation.arguments").map((raw) => {
        const entry = raw as { definitionId?: string; index?: number; value?: NodeEventValueSchema } | null;
        if (typeof entry?.definitionId !== "string" || !entry.definitionId || entry.definitionId.length > 1024
            || typeof entry.index !== "number" || !Number.isSafeInteger(entry.index) || entry.index < 0 || !entry.value)
            throw new Error("Invalid presentation construction argument binding");
        return { definitionId: entry.definitionId, index: entry.index, layout: compileLayout(entry.value, budget) };
    });
}

// A snapshot is packed only when it changes. Offsets are rebased once the
// guest allocation has an address. Spans and their elements stay contiguous.
export function packWasmArguments(bindings: readonly Binding[], construction: WasmConstruction, capacity: number) {
    let bytes = new Uint8Array(4096);
    let view = new DataView(bytes.buffer);
    const stride = 8;
    let used = bindings.length * stride;
    let readable = false;
    const pointers: number[] = [];
    const writer = {
        get view() { return view; },
        reserve(size: number, alignment: number) {
            const offset = align(used, alignment);
            const end = offset + size;
            if (end > capacity) throw new WasmPresentationValueError("Presentation construction arguments exceed their byte limit");
            used = end;
            if (used > bytes.length) {
                const grown = new Uint8Array(Math.min(capacity, Math.max(bytes.length * 2, used)));
                grown.set(bytes);
                bytes = grown;
                view = new DataView(bytes.buffer);
            }
            return offset;
        },
        pointer(at: number, offset: number) {
            view.setUint32(at, offset, true);
            if (offset) pointers.push(at);
        },
    };
    const budget = { items: 128 * 1024, bytes: capacity };
    for (const [slot, { definitionId, index, layout }] of bindings.entries()) {
        if (construction.definitionId !== definitionId) continue;
        const record = slot * stride;
        if (index >= construction.arguments.length) {
            view.setUint32(record + 4, 1, true); // IV_ARGUMENT_OMITTED
            readable = true;
            continue;
        }
        const value = construction.arguments[index];
        if (value === undefined || value === null) continue;
        const begin = used, pointerCount = pointers.length;
        const availableBudget = { ...budget };
        try {
            const offset = writer.reserve(layout.size, layout.alignment);
            layout.write(writer, offset, value, budget);
            writer.pointer(record, offset);
            view.setUint32(record + 4, 2, true); // IV_ARGUMENT_AVAILABLE
            readable = true;
        } catch (error) {
            if (!(error instanceof WasmPresentationValueError)) throw error;
            bytes.fill(0, begin, used);
            used = begin;
            pointers.length = pointerCount;
            budget.items = availableBudget.items;
            budget.bytes = availableBudget.bytes;
        }
    }
    if (!readable) return;
    return {
        size: used,
        copyTo(memory: WebAssembly.Memory, address: number) {
            const guest = new DataView(memory.buffer, address, used);
            new Uint8Array(memory.buffer, address, used).set(bytes.subarray(0, used));
            for (const at of pointers) guest.setUint32(at, address + view.getUint32(at, true), true);
        },
    };
}

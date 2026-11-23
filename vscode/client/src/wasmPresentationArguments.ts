import type { NodeEventValueSchema } from "./nodeEvents";
import { align, compileLayout, readWasmBindingSection, type Layout } from "./wasmPresentationValues";

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
    let used = bindings.length * 4;
    const pointers: number[] = [];
    const writer = {
        get view() { return view; },
        reserve(size: number, alignment: number) {
            const offset = align(used, alignment);
            used = offset + size;
            if (used > capacity) throw new Error("Presentation construction arguments exceed their byte limit");
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
        const value = construction.arguments[index];
        if (construction.definitionId !== definitionId || value === undefined || value === null) continue;
        const offset = writer.reserve(layout.size, layout.alignment);
        layout.write(writer, offset, value, budget);
        writer.pointer(slot * 4, offset);
    }
    if (!pointers.length) return;
    return {
        size: used,
        copyTo(memory: WebAssembly.Memory, address: number) {
            const guest = new DataView(memory.buffer, address, used);
            new Uint8Array(memory.buffer, address, used).set(bytes.subarray(0, used));
            for (const at of pointers) guest.setUint32(at, address + view.getUint32(at, true), true);
        },
    };
}

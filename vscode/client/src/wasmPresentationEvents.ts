import type { NodeEventSchema, NodeEventTypeId, SerializedNodeEvent } from "./nodeEvents";
import { compileLayout, readWasmBindingSection, type Layout, type Reader } from "./wasmPresentationValues";

const maximumEventItems = 4096;
const maximumStringBytes = 64 * 1024;
type Binding = { eventType: NodeEventTypeId; layout: Layout };
const invalid = (reason: string): never => { throw new Error(`Invalid presentation event: ${reason}`); };

export function readWasmEventBindings(module: WebAssembly.Module): readonly Binding[] {
    const entries = readWasmBindingSection(module, "iv.presentation.events");
    const budget = { items: 4096, bytes: maximumStringBytes };
    return entries.map((raw) => {
        const entry = raw as Partial<NodeEventSchema> | null;
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

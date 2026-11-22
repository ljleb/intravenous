// @doc docs/api/reference/presentation_c_abi.md
import type { PresentationFrame, PresentationInput, PresentationSnapshot, PresentationSavedState } from "./presentationPlugin";
import { presentationTriggerColumns, triggerColumnLimit, type TriggerColumns } from "./presentationTriggerColumns";

export const wasmPresentationAbiVersion = 4;

export function readWasmPresentationDescriptor(bytes: Uint8Array): { timeline?: true; icon?: string } {
    const magic = [0, 97, 115, 109, 1, 0, 0, 0];
    if (!magic.every((byte, index) => bytes[index] === byte)) throw new Error("Invalid WebAssembly header");
    let offset = magic.length;
    const readLength = (end: number): number => {
        let value = 0;
        for (let shift = 0; shift <= 28 && offset < end; shift += 7) {
            const byte = bytes[offset++];
            if (shift === 28 && byte > 15) break;
            value += (byte & 127) * 2 ** shift;
            if (byte < 128) return value;
        }
        throw new Error("Invalid WebAssembly section length");
    };
    const decoder = new TextDecoder("utf-8", { fatal: true });
    let descriptor: { timeline?: true; icon?: string } | undefined;
    while (offset < bytes.length) {
        const kind = bytes[offset++];
        const size = readLength(bytes.length);
        const end = offset + size;
        if (end > bytes.length) throw new Error("WebAssembly section lies outside the artifact");
        if (kind === 0) {
            const nameSize = readLength(end);
            const nameEnd = offset + nameSize;
            if (nameEnd > end) throw new Error("Invalid WebAssembly custom section name");
            if (decoder.decode(bytes.subarray(offset, nameEnd)) === "iv.presentation") {
                if (descriptor || end - nameEnd < 3 || end - nameEnd > 1027)
                    throw new Error("Invalid presentation descriptor");
                const [version, abi, flags] = bytes.subarray(nameEnd, nameEnd + 3);
                if (version !== 1 || abi !== wasmPresentationAbiVersion || flags > 1)
                    throw new Error("Unsupported presentation descriptor version or flags");
                const icon = decoder.decode(bytes.subarray(nameEnd + 3, end));
                descriptor = { timeline: flags ? true : undefined, icon: icon || undefined };
            }
        }
        offset = end;
    }
    if (!descriptor) throw new Error("Missing WASM presentation descriptor");
    return descriptor;
}

type WasmGesture = {
    id: number; phase: number; modifiers: number;
    x: number; y: number; dx: number; dy: number; wheel: number;
};
export type WasmPresentationUpdate = {
    width: number; height: number; editable: boolean; mode: PresentationSnapshot["mode"];
    gestures: readonly WasmGesture[]; connectedInputs: string[]; triggerColumns: TriggerColumns[];
};
export type WasmPresentationReply = {
    frame?: PresentationFrame; storage?: ArrayBuffer; savedState?: PresentationSavedState; error?: string; closed?: true;
};

const gesturePhases = ["begin", "move", "modifiers", "wheel", "finish", "cancel"];
export function wasmPresentationUpdate(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]): WasmPresentationUpdate {
    const gestures: WasmGesture[] = [];
    for (const input of inputs) {
        if (input.type === "interrupt") {
            gestures.push({ id: 0, phase: 5, modifiers: 0, x: 0, y: 0, dx: 0, dy: 0, wheel: 0 });
        } else if (input.type === "gesture") {
            const id = Number(input.id), phase = gesturePhases.indexOf(input.phase);
            const { modifiers, point, delta, wheel } = input;
            if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff || phase < 0)
                throw new Error("Invalid presentation gesture");
            gestures.push({ id, phase, modifiers: +modifiers.shift | (+modifiers.alt << 1)
                | (+modifiers.control << 2) | (+modifiers.meta << 3), x: point.x, y: point.y,
                dx: delta?.x ?? 0, dy: delta?.y ?? 0, wheel: wheel ?? 0 });
        }
    }
    const { width, height } = snapshot.viewport;
    return { width, height, editable: snapshot.editable, mode: snapshot.mode, gestures,
        connectedInputs: snapshot.node.ports.filter((port) => port.direction === "input" && port.connected).map(({ name }) => name),
        triggerColumns: presentationTriggerColumns(snapshot) };
}

// Also runs inside both worker hosts. All layout constants describe abi.h.
export function createWasmPresentation(bytes: string) {
    const module = new WebAssembly.Module(Uint8Array.from(atob(bytes), (byte) => byte.charCodeAt(0)));
    if (WebAssembly.Module.imports(module).length) throw new Error("Presentation WebAssembly cannot import host functions");
    const { memory, iv_alloc, iv_present, iv_create_session, iv_destroy_session, iv_save_state, iv_restore_state } =
        new WebAssembly.Instance(module).exports;
    if (!(memory instanceof WebAssembly.Memory) || typeof iv_alloc !== "function" || typeof iv_present !== "function"
        || typeof iv_create_session !== "function" || typeof iv_destroy_session !== "function"
        || typeof iv_save_state !== "function" || typeof iv_restore_state !== "function")
        throw new Error("Missing presentation ABI exports");

    const capacity = 1024 * 1024;
    const maxState = 128 * 1024;
    const maxGestures = 512;
    const inputCapacity = 128 * 1024;
    const layout = { context: 104, gesture: 56, batch: 12, polyline: 16, point: 16, target: 40,
        text: 32, observation: 40, string: 8 };
    const allocate = (size: number) => {
        if (!Number.isInteger(size) || size < 1 || size > capacity)
            throw new Error("Invalid presentation allocation size");
        const offset = iv_alloc(size);
        if (!Number.isInteger(offset) || offset <= 0 || offset % 8 !== 0 || offset + size > memory.buffer.byteLength)
            throw new Error("Presentation allocation lies outside module memory");
        return offset;
    };
    const context = allocate(layout.context);
    const gestures = allocate(maxGestures * layout.gesture), output = allocate(capacity);
    const input = allocate(inputCapacity);
    const blocks = [[context, layout.context], [gestures, maxGestures * layout.gesture], [output, capacity], [input, inputCapacity]];
    blocks.sort(([a], [b]) => a - b);
    if (blocks.some(([start], i) => i > 0 && start < blocks[i - 1][0] + blocks[i - 1][1]))
        throw new Error("Presentation allocations overlap");
    let session = iv_create_session();
    if (!Number.isInteger(session) || session <= 0 || session >= memory.buffer.byteLength
        || blocks.some(([start, length]) => session >= start && session < start + length))
        throw new Error("Invalid presentation session handle");
    const encoder = new TextEncoder(), decoder = new TextDecoder("utf-8", { fatal: true });
    const inks = [undefined, "foreground", "muted", "input", "output", "selection", "highlight", "warning", "background"] as const;
    let hasDrawing = false;

    const restore = (serialized: string) => {
        const data = encoder.encode(serialized);
        if (data.length > maxState) throw new Error("Saved presentation state exceeds the byte limit");
        new Uint8Array(memory.buffer, output, data.length).set(data);
        if (iv_restore_state(session, output, data.length) !== 0) throw new Error("Presentation could not restore state");
        hasDrawing = false;
    };

    const writeSceneInputs = (view: DataView, update: WasmPresentationUpdate) => {
        let used = 0;
        const reserve = (bytes: number) => {
            const offset = (used + 7) & ~7;
            used = offset + bytes;
            if (used > inputCapacity) throw new Error("Presentation input exceeded its byte limit");
            return input + offset;
        };
        const span = (at: number, start: number, count: number) => {
            view.setUint32(at, count ? start : 0, true);
            view.setUint32(at + 4, count, true);
        };
        const string = (at: number, value: string, maximum?: number) => {
            let bytes = encoder.encode(value);
            if (maximum !== undefined && bytes.length > maximum) {
                const clipped = new Uint8Array(maximum);
                bytes = clipped.subarray(0, encoder.encodeInto(value, clipped).written);
            }
            const start = reserve(bytes.length);
            new Uint8Array(memory.buffer, start, bytes.length).set(bytes);
            span(at, start, bytes.length);
        };
        if (update.triggerColumns.length > 8) throw new Error("Too many presentation observations");
        const observations = reserve(update.triggerColumns.length * layout.observation);
        const statuses = ["pending", "ready", "unavailable", "failed"];
        for (const [index, observation] of update.triggerColumns.entries()) {
            const at = observations + index * layout.observation;
            if (observation.columns.length > triggerColumnLimit) throw new Error("Too many observed trigger columns");
            const columns = reserve(observation.columns.length * 4);
            observation.columns.forEach((column, slot) => view.setUint32(columns + slot * 4, column, true));
            string(at, observation.id);
            span(at + 8, columns, observation.columns.length);
            view.setBigUint64(at + 16, observation.count, true);
            string(at + 24, observation.error?.slice(0, 512) ?? "", 512);
            view.setUint32(at + 32, statuses.indexOf(observation.status), true);
            view.setUint32(at + 36, +observation.partial, true);
        }
        span(context + 80, observations, update.triggerColumns.length);
        const ports = reserve(update.connectedInputs.length * layout.string);
        update.connectedInputs.forEach((name, index) => string(ports + index * layout.string, name));
        span(context + 88, ports, update.connectedInputs.length);
        view.setUint32(context + 96, ["editable", "compact", "mini", "nano"].indexOf(update.mode), true);
    };

    const writeUpdate = (update: WasmPresentationUpdate) => {
        const { width, height, editable, gestures: inputs } = update;
        if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0 || inputs.length > maxGestures)
            throw new Error("Invalid presentation viewport or gesture count");
        const view = new DataView(memory.buffer);
        for (let i = 0; i < inputs.length; ++i) {
            const input = inputs[i], base = gestures + i * layout.gesture;
            view.setUint32(base, input.id, true);
            view.setUint32(base + 4, input.phase, true);
            view.setUint32(base + 8, input.modifiers, true);
            view.setFloat64(base + 16, input.x, true);
            view.setFloat64(base + 24, input.y, true);
            view.setFloat64(base + 32, input.dx, true);
            view.setFloat64(base + 40, input.dy, true);
            view.setFloat64(base + 48, input.wheel, true);
        }
        view.setFloat64(context, width, true);
        view.setFloat64(context + 8, height, true);
        view.setUint32(context + 16, gestures, true);
        view.setUint32(context + 20, inputs.length, true);
        view.setUint32(context + 24, +editable, true);
        view.setUint32(context + 28, 0, true);
        view.setUint32(context + 32, output, true);
        view.setUint32(context + 36, capacity, true);
        new Uint8Array(memory.buffer, context + 40, layout.context - 40).fill(0);
        writeSceneInputs(view, update);
    };

    const readFrame = (): WasmPresentationReply => {
        const view = new DataView(memory.buffer);
        const uint = (at: number) => view.getUint32(at, true);
        const number = (at: number) => view.getFloat64(at, true);
        const flags = uint(context + 28), size = uint(context + 40);
        const drawing = uint(context + 48), targets = uint(context + 56), texts = uint(context + 72);
        if (uint(context + 44) || size > capacity || (flags & ~3) || ((flags & 1) && (!hasDrawing || drawing || texts)))
            throw new Error("Invalid presentation batch");
        const check = (address: number, bytes: number, alignment: number) => {
            if (address < output || address % alignment !== 0 || address + bytes > output + size)
                throw new Error("Presentation record lies outside the drawing batch");
        };
        const batches = (first: number, stride: number, limit: number, read: (address: number) => void) => {
            for (let record = first; record;) {
                check(record, layout.batch, 4);
                const address = uint(record), count = uint(record + 4), next = uint(record + 8);
                if (!count || count > limit || (next && next <= record)) throw new Error("Invalid presentation batch records");
                check(address, count * stride, 8);
                for (let index = 0; index < count; ++index) read(address + index * stride);
                limit -= count;
                record = next;
            }
        };
        const storage = drawing ? memory.buffer.slice(output, output + size) : undefined;
        const frame: PresentationFrame = {};
        const status = uint(context + 64), statusSize = uint(context + 68);
        if (statusSize) {
            if (statusSize > 512) throw new Error("Invalid presentation status size");
            check(status, statusSize, 1);
            frame.status = decoder.decode(new Uint8Array(memory.buffer, status, statusSize));
        }
        if (flags & 1) frame.reuseDrawing = true;
        else {
            const marks: NonNullable<PresentationFrame["marks"]> = [];
            let remainingVertices = 16384;
            batches(drawing, layout.polyline, 4096, (at) => {
                const id = uint(at), ink = uint(at + 4), start = uint(at + 8), count = uint(at + 12);
                if (!id || ink >= inks.length || count > 4096 || count > remainingVertices)
                    throw new Error("Invalid polyline descriptor");
                check(start, count * layout.point, 8);
                remainingVertices -= count;
                marks.push({ type: "packedPolyline", id: String(id), ink: inks[ink],
                    coordinates: new Float64Array(storage!, start - output, count * 2) });
            });
            batches(texts, layout.text, 256, (at) => {
                const id = uint(at), ink = uint(at + 4), start = uint(at + 24), length = uint(at + 28);
                if (!id || ink >= inks.length || length > 1024) throw new Error("Invalid text descriptor");
                check(start, length, 1);
                const text = decoder.decode(new Uint8Array(memory.buffer, start, length));
                marks.push({ type: "text", id: String(id), ink: inks[ink], x: number(at + 8), y: number(at + 16), text });
            });
            frame.marks = marks;
            hasDrawing = true;
        }
        frame.targets = [];
        batches(targets, layout.target, 1024, (at) => {
            const id = uint(at), drag = uint(at + 4);
            if (!id || drag > 1) throw new Error("Invalid drag target descriptor");
            frame.targets!.push({ id: String(id), drag: drag ? "absolute" : "relative",
                x: number(at + 8), y: number(at + 16), width: number(at + 24), height: number(at + 32) });
        });
        const reply: WasmPresentationReply = { frame, storage };
        if (flags & 2) {
            const count = iv_save_state(session, output, maxState);
            if (!Number.isInteger(count) || count <= 0 || count > maxState) throw new Error("Invalid serialized presentation state");
            reply.savedState = JSON.parse(decoder.decode(new Uint8Array(memory.buffer, output, count)));
        }
        return reply;
    };
    return {
        restore,
        dispose() {
            if (!session) return;
            const handle = session;
            session = 0;
            iv_destroy_session(handle);
        },
        present(update: WasmPresentationUpdate) {
            writeUpdate(update);
            iv_present(session, context);
            return readFrame();
        },
    };
}

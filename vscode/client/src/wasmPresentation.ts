import { isPresentationCurveAxis, maxTimelineCurvePoints, timelineCurvePointBytes, type PresentationCommand, type PresentationCurveAxis, type PresentationFrame,
    type PresentationInput, type PresentationSnapshot } from "./presentationPlugin";
import { maxPresentationStateBytes } from "./presentationStateJson";
import { readWasmEventBindings, wasmEventReader } from "./wasmPresentationEvents";
import { packWasmArguments, readWasmArgumentBindings, type WasmConstruction } from "./wasmPresentationArguments";
import { presentationTriggerColumns, triggerColumnLimit, type TriggerColumns } from "./presentationTriggerColumns";
import { presentationWaveforms, waveformPointBytes, type PresentationWaveform } from "./presentationWaveforms";

export const wasmPresentationAbiVersion = 11;

type WasmDescriptor = { timeline?: true; timelineSnapping?: true; icon?: string };
export function readWasmPresentationDescriptor(bytes: Uint8Array): WasmDescriptor {
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
    let descriptor: WasmDescriptor | undefined;
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
                if (version !== 1 || abi !== wasmPresentationAbiVersion || flags > 3 || flags === 2)
                    throw new Error("Unsupported presentation descriptor version or flags");
                const icon = decoder.decode(bytes.subarray(nameEnd + 3, end));
                descriptor = { timeline: flags & 1 ? true : undefined, icon: icon || undefined };
                if (flags & 2) descriptor.timelineSnapping = true;
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
    sequence: number; snapping?: { marks: string[]; playhead?: string };
    viewport?: PresentationSnapshot["viewport"];
    curveAxis?: PresentationCurveAxis;
};
type WasmControl = { id: string; phase: number; value?: number | string; sequence: number };
type WasmEditorInput = { kind: number; action: number; id?: number; sequence: number; x?: number; y?: number; value?: number };
type WasmEditReply = { id: number; revision?: number; error?: string };
const guestEditId = (session: number, id: number) => `wasm:${session}:${id}`;
export type WasmPresentationUpdate = {
    width: number; height: number; editable: boolean; mode: PresentationSnapshot["mode"];
    gestures: readonly WasmGesture[]; connectedInputs: string[]; triggerColumns: TriggerColumns[];
    waveforms: PresentationWaveform[];
    controls: WasmControl[]; arguments: Array<number | number[] | undefined>;
    editorInputs: WasmEditorInput[]; manualInputs: Array<{ id: string; value: number }>;
    clock: number;
    construction?: WasmConstruction | null;
    time?: PresentationSnapshot["viewport"]["time"]; editState: number;
    session: number; revision: number; editReplies: WasmEditReply[];
};
export type WasmPresentationReply = {
    frame?: PresentationFrame; storage?: ArrayBuffer; reuseCurvePoints?: true; error?: string; errorStack?: string; closed?: true;
};

const gesturePhases = ["begin", "move", "modifiers", "wheel", "finish", "cancel"];
const controlPhases = ["preview", "commit", "finish"];
function targetId(id: string): number {
    const value = Number(id);
    if (!Number.isInteger(value) || value <= 0 || value > 0xffffffff) throw new Error("Invalid WASM presentation target ID");
    return value;
}

export function wasmPresentationUpdate(snapshot: PresentationSnapshot, inputs: readonly PresentationInput[],
    session = 0, hostWaveforms: readonly string[] = []): WasmPresentationUpdate {
    const gestures: WasmGesture[] = [];
    const controls: WasmControl[] = [];
    const editorInputs: WasmEditorInput[] = [];
    const editReplies: WasmEditReply[] = [];
    const replyPrefix = `wasm:${session}:`;
    for (const [sequence, input] of inputs.entries()) {
        if (input.type === "interrupt") {
            editorInputs.push({ kind: 4, action: +(input.reason === "disconnected"), sequence });
        } else if (input.type === "editResult" && input.requestId.startsWith(replyPrefix)) {
            const id = Number(input.requestId.slice(replyPrefix.length));
            if (Number.isInteger(id) && id > 0 && id <= 0xffffffff)
                editReplies.push({ id, revision: input.acceptedRevision, error: input.error });
        } else if (input.type === "control") {
            const phase = controlPhases.indexOf(input.phase);
            if (phase < 0) throw new Error("Invalid presentation control");
            const value = input.value;
            controls.push({ id: input.id, phase, sequence,
                value: typeof value === "string" || (typeof value === "number" && Number.isFinite(value)) ? value : undefined });
        } else if (input.type === "gesture") {
            const id = targetId(input.id), phase = gesturePhases.indexOf(input.phase);
            const { modifiers, point, delta, wheel } = input;
            if (phase < 0) throw new Error("Invalid presentation gesture");
            gestures.push({ id, phase, modifiers: +modifiers.shift | (+modifiers.alt << 1)
                | (+modifiers.control << 2) | (+modifiers.meta << 3), x: point.x, y: point.y,
                dx: delta?.x ?? 0, dy: delta?.y ?? 0, wheel: wheel ?? 0, sequence, snapping: input.snapping,
                viewport: input.viewport ?? snapshot.viewport, curveAxis: input.curveAxis });
        } else if (input.type === "context") {
            editorInputs.push({ kind: 0, action: 0, id: targetId(input.id), sequence, ...input.point });
        } else if (input.type === "dismissContext") {
            editorInputs.push({ kind: 1, action: +!!input.cancelled, sequence });
        } else if (input.type === "items") {
            editorInputs.push({ kind: 2, action: +(input.action === "selectAll"), sequence });
        } else if (input.type === "valueView") {
            if (input.action === "pan") editorInputs.push({ kind: 3, action: 0, sequence, value: input.fraction });
            else if (input.action === "zoom") editorInputs.push({ kind: 3, action: 1, sequence, y: input.y, value: input.factor });
            else editorInputs.push({ kind: 3, action: 2, sequence });
        }
    }
    const { width, height } = snapshot.viewport;
    let argumentBudget = 4096;
    const { constructionDefinitionId, arguments: acceptedArguments } = snapshot.node;
    return { width, height, editable: snapshot.editable, mode: snapshot.mode, gestures, time: snapshot.viewport.time,
        controls, editorInputs, clock: snapshot.clockMs ?? 0, session, revision: snapshot.revision, editReplies,
        construction: constructionDefinitionId ? { definitionId: constructionDefinitionId, arguments: acceptedArguments } : null,
        arguments: acceptedArguments.slice(0, 64).map((arg) => {
            if (argumentBudget && typeof arg === "number" && Number.isFinite(arg)) {
                --argumentBudget;
                return arg;
            }
            if (Array.isArray(arg) && arg.length <= Math.min(1024, argumentBudget)
                && arg.every((value) => typeof value === "number" && Number.isFinite(value))) {
                argumentBudget -= arg.length;
                return arg;
            }
        }),
        editState: +!!snapshot.editsPending | (+!!snapshot.editError << 1),
        manualInputs: snapshot.node.ports.flatMap((port) => port.direction === "input" && !port.connected
            && Number.isFinite(port.manualValue) ? [{ id: port.name, value: port.manualValue! }] : []),
        connectedInputs: snapshot.node.ports.filter((port) => port.direction === "input" && port.connected).map(({ name }) => name),
        triggerColumns: presentationTriggerColumns(snapshot), waveforms: presentationWaveforms(snapshot, hostWaveforms) };
}

const inks = [undefined, "foreground", "muted", "input", "output", "selection", "highlight", "warning", "background"] as const;

const layout = { context: 256, gesture: 64, batch: 12, polyline: 72, point: 16, target: 64,
    text: 32, shape: 64, observation: 40, string: 8, argument: 12, control: 40, editorInput: 40,
    manualInput: 16, waveform: 32, waveformRegion: 32, editReply: 24, snapCandidates: 24 };

function sceneInputSize(update: WasmPresentationUpdate): number {
    let size = 0;
    // Each allocation starts at an 8-byte boundary. A UTF-16 code unit can
    // contribute at most three UTF-8 bytes, including lone surrogates.
    const add = (bytes: number) => { size += Math.ceil(bytes / 8) * 8; };
    const string = (value: string | undefined, limit = Infinity) => add(Math.min((value?.length ?? 0) * 3, limit));
    add(update.triggerColumns.length * layout.observation);
    for (const observation of update.triggerColumns) {
        add(observation.columns.length * 4);
        string(observation.id);
        string(observation.error, 512);
    }
    add(update.connectedInputs.length * layout.string);
    for (const name of update.connectedInputs) string(name);
    add(update.arguments.length * layout.argument);
    for (const argument of update.arguments) add((Array.isArray(argument) ? argument.length : +(argument !== undefined)) * 8);
    add(update.controls.length * layout.control);
    for (const control of update.controls) {
        string(control.id, 160);
        if (typeof control.value === "string") string(control.value, 160);
    }
    add(update.editReplies.length * layout.editReply);
    for (const reply of update.editReplies) string(reply.error, 512);
    add(update.waveforms.length * layout.waveform);
    for (const waveform of update.waveforms) {
        add(waveform.regions.length * layout.waveformRegion);
        string(waveform.id);
        string(waveform.error, 512);
        for (const region of waveform.regions) add(region.points.byteLength);
    }
    add(update.editorInputs.length * layout.editorInput);
    add(update.manualInputs.length * layout.manualInput);
    for (const port of update.manualInputs) string(port.id, 160);
    for (const { viewport, snapping } of update.gestures) {
        if (viewport?.time) add(72);
        if (!snapping) continue;
        add(layout.snapCandidates);
        add(snapping.marks.length * 8);
    }
    return size;
}

type WasmOutput = {
    uint(at: number): number;
    number(at: number): number;
    string(at: number, maximum: number): string;
    array<T>(at: number, stride: number, alignment: number, maximum: number, read: (at: number) => T): T[];
    check(at: number, size: number, alignment: number): void;
};

function readWasmPanel({ uint, number, string, array, check }: WasmOutput, panel: number): PresentationFrame["panel"] {
    if (!panel) return;
    check(panel, 16, 8);
    const rows: NonNullable<PresentationFrame["panel"]>["rows"] = [];
    array(panel + 8, 88, 8, 256, (at) => {
        const id = string(at, 160), label = string(at + 8, 1024);
        const row = uint(at + 16), kind = uint(at + 20), flags = uint(at + 24);
        if (row > rows.length || row >= 64 || kind > 3 || flags & ~3) throw new Error("Invalid presentation setting");
        const controls = rows[row] ?? (rows[row] = []);
        if (kind === 0) controls.push({ type: "text", id, text: label });
        else if (kind === 1) controls.push({ type: "knob", id, label, value: number(at + 32),
            min: number(at + 40), max: number(at + 48), defaultValue: number(at + 56), integer: !!(flags & 1),
            step: number(at + 72) || undefined, scrubSpeed: number(at + 80) || undefined });
        else if (kind === 2) controls.push({ type: "action", id, label, disabled: !!(flags & 2) });
        else {
            const options = array(at + 64, 16, 4, 64, (choice) => ({ value: string(choice, 160), label: string(choice + 8, 160) }));
            const value = options[uint(at + 28)]?.value;
            if (value === undefined) throw new Error("Invalid presentation choice selection");
            controls.push({ type: "choice", id, label, options, value });
        }
    });
    return { label: string(panel, 160), rows };
}

function readWasmTimelineGrid({ uint, number, array, check }: WasmOutput, grid: number,
    integer: (at: number) => string,
): PresentationFrame["timelineGrid"] {
    if (!grid) return;
    check(grid, 56, 8);
    const flags = uint(grid + 52), ink = uint(grid + 48);
    if (flags & ~3 || ink >= inks.length) throw new Error("Invalid presentation grid style");
    return { numerator: integer(grid), denominator: integer(grid + 8),
        strides: array(grid + 16, 8, 8, 32, integer), lineStarts: array(grid + 24, 8, 8, 32, number),
        minimumSpacing: number(grid + 32), opacity: number(grid + 40), ink: inks[ink],
        seek: !!(flags & 1), labelMajor: !!(flags & 2) };
}

// Also runs inside both worker hosts. All layout constants describe abi.h.
export function createWasmPresentation(bytes: string) {
    const module = new WebAssembly.Module(Uint8Array.from(atob(bytes), (byte) => byte.charCodeAt(0)));
    if (WebAssembly.Module.imports(module).length) throw new Error("Presentation WebAssembly cannot import host functions");
    const eventBindings = readWasmEventBindings(module);
    const argumentBindings = readWasmArgumentBindings(module);
    const { memory, iv_alloc, iv_free, iv_present, iv_create_session, iv_destroy_session, iv_save_state, iv_restore_state } =
        new WebAssembly.Instance(module).exports;
    if (!(memory instanceof WebAssembly.Memory) || typeof iv_alloc !== "function" || typeof iv_free !== "function" || typeof iv_present !== "function"
        || typeof iv_create_session !== "function" || typeof iv_destroy_session !== "function"
        || typeof iv_save_state !== "function" || typeof iv_restore_state !== "function")
        throw new Error("Missing presentation ABI exports");

    const capacity = 1024 * 1024;
    const maxGestures = 512;
    const basicInputCapacity = 128 * 1024;
    const allocate = (size: number, optional = false) => {
        if (!Number.isInteger(size) || size < 1 || size > capacity)
            throw new Error("Invalid presentation allocation size");
        const offset = iv_alloc(size);
        if (optional && offset === 0) return 0;
        if (!Number.isInteger(offset) || offset <= 0 || offset % 8 !== 0 || offset + size > memory.buffer.byteLength)
            throw new Error("Presentation allocation lies outside module memory");
        return offset;
    };
    const context = allocate(layout.context);
    const gestures = allocate(maxGestures * layout.gesture), output = allocate(capacity);
    let inputCapacity = basicInputCapacity;
    let input = allocate(inputCapacity);
    let constructionStorage = 0;
    let constructionCapacity = 0;
    let hasConstruction = false;
    let constructionSnapshot = 0;
    let curvePoints: { generation: number; count: number; points: ArrayBuffer } | undefined;
    const fixedBlocks = [[context, layout.context], [gestures, maxGestures * layout.gesture], [output, capacity]];
    const overlaps = (blocks: number[][]) => {
        blocks.sort(([a], [b]) => a - b);
        return blocks.some(([start], i) => i > 0 && start < blocks[i - 1][0] + blocks[i - 1][1]);
    };
    if (overlaps([...fixedBlocks, [input, inputCapacity]])) throw new Error("Presentation allocations overlap");
    let session = iv_create_session();
    if (!Number.isInteger(session) || session <= 0 || session >= memory.buffer.byteLength
        || [...fixedBlocks, [input, inputCapacity]].some(([start, length]) => session >= start && session < start + length))
        throw new Error("Invalid presentation session handle");
    const encoder = new TextEncoder(), decoder = new TextDecoder("utf-8", { fatal: true });
    let hasDrawing = false;

    const writeConstruction = (construction: WasmConstruction | null) => {
        constructionSnapshot = (constructionSnapshot + 1) >>> 0;
        const packed = construction && argumentBindings.length && packWasmArguments(argumentBindings, construction, capacity);
        hasConstruction = false;
        if (!packed) return;
        if (packed.size > constructionCapacity) {
            let nextCapacity = Math.min(capacity, Math.max(4096, constructionCapacity * 2, packed.size));
            if (constructionStorage) iv_free(constructionStorage);
            constructionStorage = 0;
            constructionCapacity = 0;
            let next = allocate(nextCapacity, true);
            if (!next && nextCapacity !== packed.size) {
                nextCapacity = packed.size;
                next = allocate(nextCapacity, true);
            }
            if (!next) return;
            if (overlaps([...fixedBlocks, [session, 1], [input, inputCapacity], [next, nextCapacity]]))
                throw new Error("Presentation allocations overlap");
            constructionStorage = next;
            constructionCapacity = nextCapacity;
        }
        packed.copyTo(memory, constructionStorage);
        hasConstruction = true;
    };

    const restore = (serialized: string) => {
        const data = encoder.encode(serialized);
        if (data.length > maxPresentationStateBytes) throw new Error("Saved presentation state exceeds the byte limit");
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
            let bytes: Uint8Array;
            if (maximum !== undefined && value.length * 3 > maximum) {
                const clipped = new Uint8Array(maximum);
                bytes = clipped.subarray(0, encoder.encodeInto(value, clipped).written);
            } else bytes = encoder.encode(value);
            const start = reserve(bytes.length);
            new Uint8Array(memory.buffer, start, bytes.length).set(bytes);
            span(at, start, bytes.length);
        };
        if (update.triggerColumns.length + update.waveforms.length > 8) throw new Error("Too many presentation observations");
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
        view.setUint32(context + 100, update.editState, true);
        if (update.time) {
            const { beginSample, beginFraction, spanSamples, sampleRate } = update.time;
            view.setBigInt64(context + 104, BigInt(beginSample), true);
            view.setFloat64(context + 112, beginFraction, true);
            view.setFloat64(context + 120, spanSamples, true);
            view.setUint32(context + 128, sampleRate ?? 0, true);
            view.setUint32(context + 132, +(update.time.gridMode === "beats" || update.time.gridMode === "custom"), true);
        }
        const argumentsStart = reserve(update.arguments.length * layout.argument);
        for (const [index, argument] of update.arguments.entries()) {
            const values = Array.isArray(argument) ? argument : argument === undefined ? [] : [argument];
            const at = argumentsStart + index * layout.argument;
            const start = reserve(values.length * 8);
            values.forEach((value, slot) => view.setFloat64(start + slot * 8, value, true));
            span(at, start, values.length);
            view.setUint32(at + 8, Array.isArray(argument) ? 2 : argument === undefined ? 0 : 1, true);
        }
        span(context + 136, argumentsStart, update.arguments.length);
        const controlsStart = reserve(update.controls.length * layout.control);
        for (const [index, control] of update.controls.entries()) {
            const at = controlsStart + index * layout.control;
            string(at, control.id, 160);
            view.setFloat64(at + 8, typeof control.value === "number" ? control.value : 0, true);
            view.setUint32(at + 16, control.phase, true);
            view.setUint32(at + 20, typeof control.value === "number" ? 1 : typeof control.value === "string" ? 2 : 0, true);
            if (typeof control.value === "string") string(at + 24, control.value, 160);
            else span(at + 24, 0, 0);
            view.setUint32(at + 32, control.sequence, true);
        }
        span(context + 144, controlsStart, update.controls.length);
        view.setBigUint64(context + 168, BigInt(update.revision), true);
        const replies = reserve(update.editReplies.length * layout.editReply);
        for (const [index, reply] of update.editReplies.entries()) {
            const at = replies + index * layout.editReply;
            view.setUint32(at, reply.id, true);
            view.setUint32(at + 4, +(reply.error !== undefined), true);
            view.setBigUint64(at + 8, BigInt(reply.revision ?? 0), true);
            string(at + 16, reply.error ?? "", 512);
        }
        span(context + 176, replies, update.editReplies.length);
        const waveforms = reserve(update.waveforms.length * layout.waveform);
        for (const [index, waveform] of update.waveforms.entries()) {
            const at = waveforms + index * layout.waveform;
            const regions = reserve(waveform.regions.length * layout.waveformRegion);
            string(at, waveform.id);
            span(at + 8, regions, waveform.regions.length);
            string(at + 16, waveform.error ?? "", 512);
            view.setUint32(at + 24, statuses.indexOf(waveform.status), true);
            view.setUint32(at + 28, +waveform.partial, true);
            for (const [slot, region] of waveform.regions.entries()) {
                const address = regions + slot * layout.waveformRegion;
                const points = reserve(region.points.byteLength);
                new Uint8Array(memory.buffer, points, region.points.byteLength).set(new Uint8Array(region.points));
                view.setFloat64(address, region.begin, true);
                view.setFloat64(address + 8, region.end, true);
                view.setInt32(address + 16, region.paceExponent, true);
                span(address + 20, points, region.points.byteLength / waveformPointBytes);
            }
        }
        span(context + 188, waveforms, update.waveforms.length);

        const actions = reserve(update.editorInputs.length * layout.editorInput);
        for (const [i, action] of update.editorInputs.entries()) {
            const at = actions + i * layout.editorInput;
            view.setUint32(at, action.kind, true);
            view.setUint32(at + 4, action.action, true);
            view.setUint32(at + 8, action.id ?? 0, true);
            view.setUint32(at + 12, action.sequence, true);
            view.setFloat64(at + 16, action.x ?? 0, true);
            view.setFloat64(at + 24, action.y ?? 0, true);
            view.setFloat64(at + 32, action.value ?? 0, true);
        }
        span(context + 204, actions, update.editorInputs.length);
        const manualInputs = reserve(update.manualInputs.length * layout.manualInput);
        for (const [i, port] of update.manualInputs.entries()) {
            const at = manualInputs + i * layout.manualInput;
            string(at, port.id, 160);
            view.setFloat64(at + 8, port.value, true);
        }
        span(context + 212, manualInputs, update.manualInputs.length);
        view.setFloat64(context + 224, update.clock, true);

        for (const [i, { viewport, curveAxis, snapping }] of update.gestures.entries()) {
            const at = gestures + i * layout.gesture + 56;
            view.setUint32(at, 0, true);
            view.setUint32(at + 4, 0, true);
            if (viewport?.time) {
                const { time, width, height } = viewport;
                const rate = time.sampleRate ?? 0;
                if (![width, height, time.spanSamples, time.beginFraction, rate].every(Number.isFinite)
                    || width <= 0 || height <= 0 || time.spanSamples <= 0 || time.spanSamples > 2 ** 64
                    || time.beginFraction < 0 || time.beginFraction >= 1 || !Number.isInteger(rate) || rate < 0 || rate > 0xffffffff)
                    throw new Error("Invalid presentation gesture viewport");
                if (curveAxis && !isPresentationCurveAxis(curveAxis)) throw new Error("Invalid presentation gesture value axis");
                const start = reserve(72);
                view.setBigInt64(start, BigInt(time.beginSample), true);
                view.setFloat64(start + 8, time.beginFraction, true);
                view.setFloat64(start + 16, time.spanSamples, true);
                view.setFloat64(start + 24, width, true);
                view.setFloat64(start + 32, height, true);
                view.setUint32(start + 40, rate, true);
                view.setUint32(start + 44, curveAxis ? 1 | (+curveAxis.cropped << 1) : 0, true);
                view.setFloat64(start + 48, curveAxis?.pan ?? 0, true);
                view.setFloat64(start + 56, curveAxis?.zoom ?? 1, true);
                view.setFloat64(start + 64, curveAxis?.center ?? .5, true);
                view.setUint32(at + 4, start, true);
            }
            if (!snapping) continue;
            const candidates = reserve(layout.snapCandidates);
            const marks = reserve(snapping.marks.length * 8);
            snapping.marks.forEach((mark, index) => view.setBigInt64(marks + index * 8, BigInt(mark), true));
            span(candidates, marks, snapping.marks.length);
            view.setBigInt64(candidates + 8, BigInt(snapping.playhead ?? 0), true);
            view.setUint32(candidates + 16, +(snapping.playhead !== undefined), true);
            view.setUint32(at, candidates, true);
        }
    };

    const writeUpdate = (update: WasmPresentationUpdate) => {
        const { width, height, editable, gestures: inputs } = update;
        if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0
            || inputs.length + update.controls.length + update.editorInputs.length + update.editReplies.length > maxGestures
            || !Number.isSafeInteger(update.revision) || update.revision < 0)
            throw new Error("Invalid presentation viewport or gesture count");
        const required = sceneInputSize(update);
        if (required > inputCapacity && inputCapacity < capacity) {
            const nextCapacity = Math.min(capacity, Math.ceil(required / 65536) * 65536);
            iv_free(input);
            const next = allocate(nextCapacity);
            const otherBlocks = constructionStorage ? [[constructionStorage, constructionCapacity]] : [];
            if (overlaps([...fixedBlocks, [next, nextCapacity], [session, 1], ...otherBlocks]))
                throw new Error("Presentation allocations overlap");
            input = next;
            inputCapacity = nextCapacity;
        }
        if (update.construction !== undefined) writeConstruction(update.construction);
        const view = new DataView(memory.buffer);
        for (let i = 0; i < inputs.length; ++i) {
            const input = inputs[i], base = gestures + i * layout.gesture;
            view.setUint32(base, input.id, true);
            view.setUint32(base + 4, input.phase, true);
            view.setUint32(base + 8, input.modifiers, true);
            view.setUint32(base + 12, input.sequence, true);
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
        view.setUint32(context + 248, constructionSnapshot, true);
        if (hasConstruction) {
            view.setUint32(context + 196, constructionStorage, true);
            view.setUint32(context + 200, argumentBindings.length, true);
        }
    };

    const readFrame = (workerSession: number): WasmPresentationReply => {
        const view = new DataView(memory.buffer);
        const uint = (at: number) => view.getUint32(at, true);
        const number = (at: number) => view.getFloat64(at, true);
        const flags = uint(context + 28), size = uint(context + 40);
        const drawing = uint(context + 48), targets = uint(context + 56), texts = uint(context + 72);
        const grid = uint(context + 164), shapes = uint(context + 240);
        if (uint(context + 44) || size > capacity || (flags & ~3) || ((flags & 1) && (!hasDrawing || drawing || texts || grid || shapes)))
            throw new Error("Invalid presentation batch");
        const check = (address: number, bytes: number, alignment: number) => {
            if (address < output || address % alignment !== 0 || address + bytes > output + size)
                throw new Error("Presentation record lies outside the drawing batch");
        };
        const string = (at: number, maximum: number) => {
            const start = uint(at), length = uint(at + 4);
            if (length > maximum) throw new Error("Presentation string exceeded its limit");
            if (!length) return "";
            check(start, length, 1);
            return decoder.decode(new Uint8Array(memory.buffer, start, length));
        };
        const array = <T>(at: number, stride: number, alignment: number, maximum: number, read: (at: number) => T) => {
            const start = uint(at), count = uint(at + 4);
            if (count > maximum) throw new Error("Presentation span exceeded its limit");
            if (!count) return [];
            check(start, count * stride, alignment);
            return Array.from({ length: count }, (_, index) => read(start + index * stride));
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
        const curve = uint(context + 252);
        if (curve) {
            check(curve, 64, 8);
            const start = uint(curve), count = uint(curve + 4), generation = uint(curve + 32);
            const curveFlags = uint(curve + 36), byteLength = count * timelineCurvePointBytes;
            if (count > maxTimelineCurvePoints || curveFlags & ~7
                || (count && (!start || start % 8 || start + byteLength > view.byteLength)))
                throw new Error("Invalid presentation timeline curve");
            if (curvePoints?.generation !== generation) {
                const selectionTarget = uint(curve + 40), additionTarget = uint(curve + 44);
                const targets = new Set([selectionTarget, additionTarget]);
                const marks = new Set<number>();
                let previous: bigint | undefined;
                let previousTension = 0;
                for (let i = 0; i < count; ++i) {
                    const at = start + i * timelineCurvePointBytes;
                    const sample = view.getBigInt64(at, true);
                    const value = view.getFloat32(at + 24, true), tension = view.getFloat32(at + 28, true);
                    const target = uint(at + 32), mark = uint(at + 36), tensionTarget = uint(at + 40), tensionMark = uint(at + 44);
                    const interpolation = uint(at + 48);
                    if ((previous !== undefined && (sample < previous || (sample === previous && previousTension)))
                        || ![number(at + 8), number(at + 16), value, tension].every(Number.isFinite)
                        || tension < 0 || tension > 1 || interpolation > 2 || uint(at + 52) & ~3
                        || !target || targets.has(target) || !mark || mark >= 0xffffffff || marks.has(mark) || marks.has(mark + 1)
                        || (!!tensionTarget !== !!tensionMark))
                        throw new Error("Invalid presentation timeline curve point");
                    targets.add(target);
                    marks.add(mark); marks.add(mark + 1);
                    if (tensionTarget) {
                        if (interpolation !== 2 || targets.has(tensionTarget) || tensionMark >= 0xffffffff
                            || marks.has(tensionMark) || marks.has(tensionMark + 1))
                            throw new Error("Invalid presentation timeline curve handle");
                        targets.add(tensionTarget);
                        marks.add(tensionMark); marks.add(tensionMark + 1);
                    }
                    previous = sample;
                    previousTension = tensionTarget;
                }
                curvePoints = { generation, count,
                    points: count ? memory.buffer.slice(start, start + byteLength) : new ArrayBuffer(0) };
            } else if (curvePoints.count !== count) throw new Error("Presentation curve generation changed size");
            frame.timelineCurve = { points: curvePoints.points,
                pan: number(curve + 8), zoom: number(curve + 16), center: number(curve + 24),
                cropped: !!(curveFlags & 1), editable: !!(curveFlags & 2), viewportDrawing: !!(curveFlags & 4),
                selectionTarget: uint(curve + 40), additionTarget: uint(curve + 44),
                minimumInput: string(curve + 48, 160), maximumInput: string(curve + 56, 160) };
        }
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
                const style = uint(at + 16), target = uint(at + 20);
                if (!id || ink >= inks.length || count > 4096 || count > remainingVertices || style & ~15)
                    throw new Error("Invalid polyline descriptor");
                check(start, count * layout.point, 8);
                remainingVertices -= count;
                const strokeWidth = number(at + 32);
                marks.push({ type: "packedPolyline", id: String(id), ink: inks[ink],
                    ...(target ? { target: String(target) } : {}), ...(style & 1 ? { opacity: number(at + 24) } : {}),
                    ...(strokeWidth ? { strokeWidth } : {}), ...(style & 2 ? { dashed: true } : {}),
                    ...(style & 4 ? { fill: true } : {}), ...(style & 8 ? { fade: {
                        from: { x: number(at + 40), y: number(at + 48) }, to: { x: number(at + 56), y: number(at + 64) },
                    } } : {}),
                    coordinates: new Float64Array(storage!, start - output, count * 2) });
            });
            batches(shapes, layout.shape, 1024, (at) => {
                const id = uint(at), kind = uint(at + 4), ink = uint(at + 8), border = uint(at + 12), target = uint(at + 16);
                if (!id || kind > 1 || ink >= inks.length || border >= inks.length) throw new Error("Invalid presentation shape");
                const x = number(at + 24), y = number(at + 32), width = number(at + 40), height = number(at + 48);
                const common = { id: String(id), ink: inks[ink], border: inks[border], opacity: number(at + 56),
                    target: target ? String(target) : undefined, x, y };
                marks.push(kind === 0 ? { type: "rect", ...common, width, height } : { type: "circle", ...common, radius: width });
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
            const id = uint(at), drag = uint(at + 4), options = uint(at + 40);
            if (!id || drag > 1 || options & ~63) throw new Error("Invalid drag target descriptor");
            frame.targets!.push({ id: String(id), drag: drag ? "absolute" : "relative",
                button: options & 1 ? "secondary" : undefined, wheel: options & 2 ? true : undefined,
                context: options & 4 ? true : undefined, cursorReturn: options & 8 ? true : undefined,
                hideCursor: options & 16 ? true : undefined, cursor: options & 32 ? "ew-resize" : undefined,
                hint: string(at + 44, 512) || undefined, shortcuts: string(at + 52, 512) || undefined,
                x: number(at + 8), y: number(at + 16), width: number(at + 24), height: number(at + 32) });
        });
        const outputReader = { uint, number, string, array, check };
        frame.panel = readWasmPanel(outputReader, uint(context + 152));
        frame.contextMenu = readWasmPanel(outputReader, uint(context + 232));
        const feedback = uint(context + 236);
        if (feedback) {
            check(feedback, 64, 8);
            const options = uint(feedback), cursor = uint(feedback + 4);
            if (options & ~31 || cursor > 2) throw new Error("Invalid presentation editor feedback");
            if (options & 1) frame.itemEditing = true;
            if (options & 2) frame.valueNavigation = true;
            if (options & 4) frame.cancelDrag = true;
            if (options & 8) frame.wakeAfterMs = number(feedback + 48);
            if (options & 16) frame.secondsRulerInset = number(feedback + 56);
            const hint = string(feedback + 32, 512), shortcuts = string(feedback + 40, 512);
            if (hint || shortcuts || cursor) frame.drag = { hint: hint || undefined, shortcuts: shortcuts || undefined,
                cursor: cursor === 2 ? { sample: view.getBigInt64(feedback + 8, true).toString(), yFraction: number(feedback + 24) }
                    : cursor === 1 ? { x: number(feedback + 16), y: number(feedback + 24) } : undefined };
        }
        const edits = uint(context + 156);
        if (edits) {
            const readEvent = wasmEventReader(eventBindings, { view, check });
            const commands: PresentationCommand[] = [];
            const requestIds = new Set<number>();
            for (let edit = edits; edit;) {
                check(edit, 24, 8);
                const binding = uint(edit), gestureId = uint(edit + 4), request = uint(edit + 8);
                const editFlags = uint(edit + 12), payload = uint(edit + 16), next = uint(edit + 20);
                if (commands.length === 32 || (next && next <= edit)
                    || (request && requestIds.has(request))) throw new Error("Invalid presentation edit sequence");
                if (request) requestIds.add(request);
                const gesture = gestureId ? guestEditId(workerSession, gestureId) : undefined;
                const requestId = request ? guestEditId(workerSession, request) : undefined;
                if (editFlags === 2) {
                    if (!gesture || binding || payload) throw new Error("Invalid presentation edit finish");
                    commands.push({ type: "finish", gesture, requestId });
                } else {
                    if (editFlags > 1 || (editFlags === 1 && (!gesture || requestId)))
                        throw new Error("Invalid presentation edit flags");
                    commands.push({ type: "edit", gesture, requestId, replacePending: editFlags === 1 || undefined,
                        event: readEvent(binding, payload) });
                }
                edit = next;
            }
            if (uint(context + 184) !== commands.length) throw new Error("Invalid presentation edit count");
            frame.commands = commands;
        } else if (uint(context + 184)) throw new Error("Invalid presentation edit count");
        frame.timelineGrid = readWasmTimelineGrid(outputReader, uint(context + 164), (at) => view.getBigUint64(at, true).toString());
        if (flags & 2) {
            const count = iv_save_state(session, output, maxPresentationStateBytes);
            if (!Number.isInteger(count) || count <= 0 || count > maxPresentationStateBytes)
                throw new Error("Invalid serialized presentation state");
            frame.saveRequest = decoder.decode(new Uint8Array(memory.buffer, output, count));
        }
        return { frame, storage };
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
            return readFrame(update.session);
        },
    };
}

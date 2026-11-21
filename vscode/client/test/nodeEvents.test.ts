import test from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { AddPoint, MovePoint } from "../src/generated/automationEvents";
import { defineNodeEvent, encodeNodeEventValue, serializeNodeEvent, validateNodeEventTarget,
    type NodeEvent, type NodeEventProtocol, type NodeEventValueSchema } from "../src/nodeEvents";
import { generateNodeEventBindings } from "../src/generateNodeEventBindings";

const target = { instanceId: "curve" };
const event = serializeNodeEvent(MovePoint({ id: 4, seconds: 0, value: .5, sample_index: "96000" }));
const pointSchema: NodeEventValueSchema = { kind: "aggregate", fields: [
    { name: "id", value: { kind: "unsignedInteger", bitWidth: 32 } },
    { name: "seconds", value: { kind: "floatingPoint", bitWidth: 64 } },
    { name: "value", value: { kind: "floatingPoint", bitWidth: 32 } },
    { name: "sample_index", value: { kind: "convertedScalar" } },
] };
const protocol = (): NodeEventProtocol => ({ formatVersion: 0, definitions: [],
    eventTypes: [{ ...event.eventType, displayName: "diagnostic spelling can change", payload: pointSchema }],
    receivers: [{ address: target, supportedEventTypes: [event.eventType] }] });

test("typed automation events snapshot their payload and cannot be forged as a raw JSON event", () => {
    const point = { id: 4, seconds: 0, value: .5, interpolation: "bezier" as const, tension: 1, sample_index: "96000", order: 0 };
    const typed = AddPoint({ point });
    point.value = .9;
    const first = serializeNodeEvent(typed);
    assert.deepEqual(first.payload, { point: { ...point, value: .5 } });
    (first.payload as { point: { value: number } }).point.value = 1;
    assert.deepEqual(serializeNodeEvent(typed).payload, { point: { ...point, value: .5 } });
    assert.throws(() => serializeNodeEvent({} as NodeEvent), /typed event/);
    // These calls must remain type errors even though the wire payload is JSON.
    const typeErrorsOnly = () => {
        // @ts-expect-error payload fields use the compiler's declared types
        MovePoint({ id: "four", seconds: 0, value: .5, sample_index: "96000" });
        // @ts-expect-error typed event values carry a factory-only brand
        const raw: NodeEvent = { id: 4, seconds: 2, value: .5 };
        return raw;
    };
    void typeErrorsOnly;
});

test("codecs reject invalid numeric values, missing fields, and fixed sequence lengths", () => {
    for (const value of [-1, 2 ** 32, NaN, Infinity])
        assert.throws(() => MovePoint({ id: value, seconds: 0, value: .5, sample_index: "96000" }));
    assert.throws(() => MovePoint({ id: 4, seconds: Infinity, value: .5, sample_index: "96000" }), /finite/);
    assert.throws(() => MovePoint({ id: 4, seconds: 0, value: 1e100, sample_index: "96000" }), /floating-point range/);
    assert.throws(() => encodeNodeEventValue(pointSchema, { id: 4, seconds: 2 }), /payload.value/);
    assert.throws(() => encodeNodeEventValue(pointSchema, { ...event.payload as object, surprise: true }), /unknown field/);
    assert.throws(() => encodeNodeEventValue({ kind: "unsignedInteger", bitWidth: 64 }, 2 ** 53), /exactly representable/);
    assert.throws(() => encodeNodeEventValue({ kind: "sequence", fixedElementCount: 2,
        element: { kind: "boolean" } }, [true]), /fixed sequence length/);
    assert.equal(encodeNodeEventValue({ kind: "enumeration", enumerators: [{ name: "linear", value: "0" }] }, "linear"), "linear");
});

test("compiler enum signedness survives binding generation and factories use enumerator names", () => {
    for (const signed of [false, true]) {
        const schema: NodeEventValueSchema = { kind: "enumeration", bitWidth: 8, signed,
            enumerators: [{ name: "empty", value: "0" }, { name: "bezier", value: signed ? "-1" : "2" }] };
        const id = { nominalId: "curve-policy", definitionFingerprint: "enum-schema" };
        const factory = defineNodeEvent<"empty" | "bezier">(id, schema);
        assert.equal(serializeNodeEvent(factory("bezier")).payload, "bezier");
        assert.throws(() => encodeNodeEventValue(schema, "unknown"), /enumerator/);
        const generated = generateNodeEventBindings({ formatVersion: 0, definitions: [],
            eventTypes: [{ ...id, displayName: "Curve policy", payload: schema }] }, { CurvePolicy: id });
        assert.ok(generated.includes(`"signed":${signed}`));
    }
});

test("dispatch validates exact receiver support, independent of diagnostic names and empty path spelling", () => {
    validateNodeEventTarget(protocol(), { ...target, path: [] }, event);
    const updated = protocol();
    updated.eventTypes[0].definitionFingerprint = "changed";
    assert.throws(() => validateNodeEventTarget(updated, target, event), /definition changed/);
    const removed = protocol(); removed.receivers = [];
    assert.throws(() => validateNodeEventTarget(removed, target, event), /no longer supports/);
    const unsupported = protocol(); unsupported.receivers![0].supportedEventTypes = [];
    assert.throws(() => validateNodeEventTarget(unsupported, target, event), /no longer supports/);
    const version = protocol(); version.formatVersion = 1;
    assert.throws(() => validateNodeEventTarget(version, target, event), /protocol version/);
});

test("binding generation uses nominal identities, detects export collisions, and reproduces the bundled binding", () => {
    const root = path.resolve(__dirname, "../../bindings");
    const snapshot = JSON.parse(fs.readFileSync(path.join(root, "automation.node-events.json"), "utf8"));
    const names = JSON.parse(fs.readFileSync(path.join(root, "automation.names.json"), "utf8"));
    assert.equal(generateNodeEventBindings(snapshot, names),
        fs.readFileSync(path.resolve(__dirname, "../../src/generated/automationEvents.ts"), "utf8"));
    const input = protocol();
    assert.throws(() => generateNodeEventBindings(input, { class: event.eventType.nominalId }), /collision/);
    assert.throws(() => generateNodeEventBindings(input, { Event: event.eventType.nominalId,
        EventPayload: event.eventType.nominalId }), /collision/);
    const ambiguous = protocol();
    ambiguous.eventTypes.push({ ...ambiguous.eventTypes[0], definitionFingerprint: "another" });
    assert.throws(() => generateNodeEventBindings(ambiguous, { Move: event.eventType.nominalId }), /exactly one/);
    assert.match(generateNodeEventBindings(ambiguous, { Move: event.eventType }), /defineNodeEvent/);
    assert.throws(() => defineNodeEvent(event.eventType, { kind: "unsupported", reason: "NodeRef cannot cross JSON" })({}), /NodeRef/);
});

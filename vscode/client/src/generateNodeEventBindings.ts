import * as fs from "node:fs";
import type { NodeEventProtocol, NodeEventSchema, NodeEventValueSchema } from "./nodeEvents";

// Export names are frontend choices. Values select stable nominal identities,
// never diagnostic display names. A fingerprint is required if the input keeps
// several definitions of one nominal type.
export type NodeEventBindingNames = Record<string, string | { nominalId: string; definitionFingerprint: string }>;

function payloadType(schema: NodeEventValueSchema): string {
    switch (schema.kind) {
        case "boolean": return "boolean";
        case "string": return "string";
        case "signedInteger": case "unsignedInteger": case "floatingPoint": return "number";
        case "convertedScalar": return "boolean | number | string";
        case "enumeration": return schema.enumerators?.map((entry) => JSON.stringify(entry.name)).join(" | ") || "never";
        case "nodeAddress": return "NodeAddress";
        case "aggregate": return `{ ${(schema.fields ?? []).map((field) =>
            `readonly ${JSON.stringify(field.name)}: ${payloadType(field.value)}`).join("; ")} }`;
        case "sequence":
            if (!schema.element) throw new Error("Sequence has no element schema");
            return `ReadonlyArray<${payloadType(schema.element)}>${schema.fixedElementCount
                ? ` & { readonly length: ${schema.fixedElementCount} }` : ""}`;
        default: throw new Error(schema.reason ?? "Unsupported event payload type");
    }
}

export function generateNodeEventBindings(protocol: NodeEventProtocol, names: NodeEventBindingNames, runtimeImport = "../nodeEvents"): string {
    if (protocol.formatVersion !== 0) throw new Error("Unsupported node-event protocol version");
    const selected: Array<{ name: string; schema: NodeEventSchema }> = [];
    const reserved = new Set(["NodeAddress", "defineNodeEvent", "break", "case", "catch", "class", "const",
        "continue", "debugger", "default", "delete", "do", "else", "enum", "export", "extends", "false", "finally",
        "for", "function", "if", "import", "in", "instanceof", "interface", "let", "new", "null", "return", "super",
        "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield", "await",
        "implements", "package", "private", "protected", "public", "static", "type", "namespace", "declare", "eval", "arguments"]);
    for (const [name, selector] of Object.entries(names)) {
        if (!/^[A-Za-z_$][\w$]*$/.test(name)) throw new Error(`Invalid frontend event export '${name}'`);
        // Each export also creates a payload type; catch collisions regardless
        // of the order in which the frontend names were supplied.
        for (const symbol of [name, `${name}Payload`]) {
            if (reserved.has(symbol)) throw new Error(`Frontend event export collision '${symbol}'`);
            reserved.add(symbol);
        }
        const nominalId = typeof selector === "string" ? selector : selector.nominalId;
        const matches = protocol.eventTypes.filter((event) => event.nominalId === nominalId
            && (typeof selector === "string" || event.definitionFingerprint === selector.definitionFingerprint));
        if (matches.length !== 1) throw new Error(`Event '${name}' needs exactly one matching nominal identity and fingerprint`);
        selected.push({ name, schema: matches[0] });
    }
    const usesAddress = selected.some(({ schema }) => /\bNodeAddress\b/.test(payloadType(schema.payload)));
    return [
        "// Generated from the compiler's neutral node-event protocol. Do not edit.",
        `import { defineNodeEvent${usesAddress ? ", type NodeAddress" : ""} } from ${JSON.stringify(runtimeImport)};`,
        "",
        ...selected.flatMap(({ name, schema }) => [
            `export type ${name}Payload = ${payloadType(schema.payload)};`,
            `export const ${name} = defineNodeEvent<${name}Payload>(${JSON.stringify({ nominalId: schema.nominalId,
                definitionFingerprint: schema.definitionFingerprint })}, ${JSON.stringify(schema.payload)});`, "",
        ]),
    ].join("\n");
}

if (require.main === module) {
    const [protocolFile, namesFile, outputFile, runtimeImport] = process.argv.slice(2);
    if (!protocolFile || !namesFile || !outputFile) {
        throw new Error("Usage: generateNodeEventBindings PROTOCOL.json NAMES.json OUTPUT.ts [RUNTIME_IMPORT]");
    }
    fs.writeFileSync(outputFile, generateNodeEventBindings(JSON.parse(fs.readFileSync(protocolFile, "utf8")),
        JSON.parse(fs.readFileSync(namesFile, "utf8")), runtimeImport));
}

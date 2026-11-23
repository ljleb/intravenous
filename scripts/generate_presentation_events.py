#!/usr/bin/env python3
"""Generate fixed C++ presentation bindings from a node-event protocol."""
import argparse
import json
from pathlib import Path
import re


IDENTIFIER = re.compile(r"[A-Za-z_]\w*", re.ASCII)
WIDTHS = (8, 16, 32, 64)


def identifier(value):
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise ValueError(f"Invalid C++ identifier {value!r}")
    return value


def integer_type(schema, signed):
    bits = schema.get("bitWidth")
    if bits not in WIDTHS:
        raise ValueError(f"Unsupported integer width {bits}")
    return f"{'' if signed else 'u'}int{bits}_t"


class Bindings:
    def __init__(self, namespace, event_names):
        self.namespace = namespace
        self.types = []
        self.records = []
        self.names = set(event_names)

    def name(self, suggestion):
        candidate = suggestion
        suffix = 2
        while candidate in self.names:
            candidate = f"{suggestion}{suffix}"
            suffix += 1
        self.names.add(candidate)
        return candidate

    def type(self, schema, suggestion, fixed_name=None):
        kind = schema["kind"]
        if kind == "boolean":
            return "bool"
        if kind == "floatingPoint":
            bits = schema.get("bitWidth")
            if bits not in (32, 64):
                raise ValueError(f"Unsupported floating point width {bits}")
            return "float" if bits == 32 else "double"
        if kind in ("signedInteger", "unsignedInteger"):
            return integer_type(schema, kind == "signedInteger")
        if kind in ("string", "convertedScalar"):
            return "IvString" if kind == "string" else "IvConvertedScalar"
        if kind == "sequence":
            element = self.type(schema["element"], suggestion + "Element")
            return f"::iv::presentation::Span<{element} const>"
        if kind == "enumeration":
            name = self.name(suggestion)
            storage = integer_type(schema, schema.get("signed") is True)
            variants = []
            # Use a hex unsigned literal so INT64_MIN also has a representable
            # token before converting it to its declared enum storage type.
            for entry in schema["enumerators"]:
                value = int(entry["value"])
                bits = schema["bitWidth"]
                signed = schema.get("signed") is True
                minimum = -(1 << (bits - 1)) if signed else 0
                if not minimum <= value < 1 << (bits - signed):
                    raise ValueError(f"Enumerator {entry['name']} is out of range")
                literal = hex(value & ((1 << bits) - 1)) + "ULL"
                variants.append(f"    {identifier(entry['name'])} = static_cast<{storage}>({literal}),")
            self.types.append(f"enum class {name} : {storage} {{\n" + "\n".join(variants) + "\n};")
            return name
        if kind != "aggregate":
            raise ValueError(schema.get("reason") or f"Unsupported presentation event field {kind}")

        name = fixed_name or self.name(suggestion)
        declarations = []
        retained = []
        for field in schema["fields"]:
            field_name = identifier(field["name"])
            field_type = self.type(field["value"], name + field_name[0].upper() + field_name[1:])
            declarations.append(f"    {field_type} {field_name};")
            if field["value"]["kind"] in ("string", "convertedScalar", "sequence", "aggregate"):
                retained.append(f"arena.retain(value.{field_name})")
        self.types.append(f"struct {name} {{\n" + "\n".join(declarations) + "\n};")
        if retained:
            qualified = f"::{self.namespace}::{name}"
            expression = " &&\n            ".join(retained)
            self.records.append(f"""template<> struct EventRecord<{qualified}> {{
    static bool retain(EditArena const& arena, {qualified}& value)
    {{
        return {expression};
    }}
}};""")
        return name


def generate(protocol, names, namespace):
    if protocol.get("formatVersion") != 0:
        raise ValueError("Unsupported node-event protocol version")
    namespace = "::".join(identifier(part) for part in namespace.split("::"))
    if not 1 <= len(names) <= 64:
        raise ValueError("A presentation must bind between 1 and 64 events")
    bindings = Bindings(namespace, (identifier(name) for name in names))
    metadata = []
    entries = []
    for index, (name, selector) in enumerate(names.items()):
        nominal = selector if isinstance(selector, str) else selector["nominalId"]
        candidates = [schema for schema in protocol["eventTypes"] if schema["nominalId"] == nominal
                      and (isinstance(selector, str)
                           or schema["definitionFingerprint"] == selector["definitionFingerprint"])]
        if len(candidates) != 1:
            raise ValueError(f"Event {name!r} requires a unique nominal identity and schema fingerprint")
        entry = candidates[0]
        if entry["payload"]["kind"] != "aggregate":
            raise ValueError(f"Event {name!r} must have a record payload")
        metadata.append({key: entry[key] for key in ("nominalId", "definitionFingerprint", "payload")})
        bindings.type(entry["payload"], name, fixed_name=name)
        entries.append(f"template<> struct EventBinding<::{namespace}::{name}> {{\n"
                       f"    static constexpr uint32_t index = {index};\n}};")
    section = json.dumps(metadata, ensure_ascii=True, separators=(",", ":"))
    if len(section) > 65536:
        raise ValueError("Presentation event schema section exceeds 64 KiB")
    assembly = '.section .custom_section.iv.presentation.events,"",@\n.ascii ' + json.dumps(section) + "\n.text\n"
    return "\n".join([
        "// Generated from the compiler's node-event protocol. Do not edit.",
        "#pragma once", "#include <intravenous/presentation/presentation.hpp>", "",
        f"namespace {namespace} {{", "\n\n".join(bindings.types), "}", "",
        "namespace iv::presentation::detail {", "\n\n".join(bindings.records + entries), "}", "",
        "#if defined(__wasm32__)", f"__asm__({json.dumps(assembly)});", "#endif", "",
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("protocol", type=Path)
    parser.add_argument("names", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--namespace", required=True)
    args = parser.parse_args()
    try:
        source = generate(json.loads(args.protocol.read_text()), json.loads(args.names.read_text()), args.namespace)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(source)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f"Presentation event binding failed: {error}\n")


if __name__ == "__main__":
    main()

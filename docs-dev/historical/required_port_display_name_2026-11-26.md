# Required port display names

This section came from the LLVM node reference. It was superseded on
2026-11-26 when display names became optional and their argument order
changed to short followed by long.
[Current port declaration reference](../api/reference/llvm_nodes.md)

An authored port declares an identifier, a display name, and an optional
short name. For example, `iv::input<iv::Sample>({"phase_offset", "Phase offset",
"Phase"})` declares all three. Callbacks and connections use the identifier.
Use lowercase ASCII letters, digits, and single underscores. Start with a
letter and end with a letter or digit.

Display names and short names allow UTF-8 text and punctuation. Use one
line, with single spaces and no space at either end. The editor reserves
` | `, ` · `, and ` → ` with a space on each side. Non-breaking spaces count as
spaces here. **A|B**, **N·m**, and **Hz→MIDI** are valid port names.
**A | B** and **Hz → MIDI** are reserved forms.

For a module input, write `auto const dry = g.input({"Dry signal", "Dry"});`.
The identifier comes from the variable name. For a module output, write
`g.outputs("wet"_P.with_display_name("Wet signal", "Wet") = wet_signal);`.
Generated positional ports can keep their index-based identity. Leaf code
can express that with `iv::PortNames::generated()`. A module that exports
a single expression with `g.outputs(expression)` generates the output
identifier `value` and display name **Value**.

[Earlier optional-label API](./optional_port_display_name_2026-11-25.md)

# Modules Follow-Up Notes

## Source lifecycle and tooling

- Explore one-promotion runtime PGO for hot-reloadable module definitions. The
  discovery, safety constraints, and proposed ownership are recorded in
  [Runtime PGO For Hot-Reloadable IV Modules](iv_module_runtime_pgo_direction.md).
- Give newly created local module sources a predefined, identical
  `compile_commands.json`.
- Investigate why clangd does not reliably reload its compilation database after
  the first module build.
- Define smooth source-deletion behavior: deleting a source must remove it from
  the project immediately and give its instances a defined, non-crashing
  transition.

## Modules panel and instances

- Make source and instance rows more compact without losing their actions.
- Rename instance `Select` to `Open`.
- Add a non-unique user-facing instance name so users do not need to identify
  instances by UUID.

## Port configuration and controls

- Add optional sample-input min/max configuration, defaulting to `-inf` and
  `+inf`. Node `tick()` / `tick_block()` implementations may enforce clamping
  where required.
- Use range-aware knob mappings:
  - finite range: linear;
  - both bounds infinite: `x / (1 - x^2)`, centered by default;
  - only upper bound infinite: `x^2 / (1 - x)`;
  - only lower bound infinite: `x^2 / (1 + x)`.
- Keep source provenance on named reference identifiers rather than direct
  `g.input(...)`/node expressions; runtime sidepanel controls do not depend on
  a port having its own source span.
- Audit whether explicitly disconnecting a `g.input()` port is already possible
  or needs a new port-state transition.

### Settled design decisions

- `InputConfig::min` and `InputConfig::max` are `std::optional<Sample>`;
  `nullopt` means unbounded. They are control metadata, not a global DSP
  clamping policy. Nodes may clamp in `tick()` / `tick_block()` where that is
  semantically required.
- `g.input()` accepts both named and unnamed configured forms:
  `g.input("name", default, min, max)` and `g.input(default, min, max)`.
- Knobs always retain a normalized position in `[0, 1]`; the port value is
  mapped separately. Finite bounds are linear. With a finite lower bound use
  `min + x|x| / (1 - x)`; with a finite upper bound use
  `max - x|x| / (1 - x)`. With neither bound, map
  `u = 2x - 1` through `u / (1 - u^2)` around the configured default, so the
  default is exactly at knob center.
- Use stable inverses for the one-sided and unbounded mappings. Incoming values
  outside an inverse's domain clamp only the rendered knob position; they do
  not rewrite the actual port value. The next user interaction applies the
  forward mapping and deliberately brings the value back into the control
  range.
- VST wrapper parameter inputs expose denormalized plugin default/min/max via
  `InputConfig`; runtime conversion back to JUCE normalized values uses the
  parameter's `RangedAudioParameter` conversion.
- Source annotations are attached to declarator identifiers and later
  id-expression references for source-annotatable node/sample/event/public
  input refs. A direct `g.input(...)` or node expression is not itself a
  source-active region. Named `_P`/`_F` bindings contribute only their quoted
  name string, and that span belongs to the corresponding virtual input port
  rather than the enclosing virtual node.
- Source identity groups repeated annotated `g.input()` calls, including calls
  made in loops, into one logical public input with concrete members.

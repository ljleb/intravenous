# Modules Follow-Up Notes

## Source lifecycle and tooling

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
- Later add `Open lanes view`, which upserts a lane view filtered to one
  instance.
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
- Add source annotations for sample/event port refs, parallel to node-ref
  annotations, so `g.input(...)` can be exposed as a sidepanel control.
- Audit whether explicitly disconnecting a `g.input()` lane is already possible
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
- Do not generalize source annotations to arbitrary output port refs.
  `g.input()` returns a dedicated `PublicSampleInputRef`, implicitly
  convertible to `SamplePortRef`; only that type receives LLVM source
  annotations.
- Source identity groups repeated annotated `g.input()` calls, including calls
  made in loops, into one logical public input with concrete members. The
  default behavior is a shared logical lane for all members.
- Public-input connectivity follows the existing sample-input model:
  logical state selects the shared logical lane or disconnects; concrete
  members default to logical-follow and may override to a concrete lane or
  disconnected. The sidebar should present the same logical/member hierarchy.

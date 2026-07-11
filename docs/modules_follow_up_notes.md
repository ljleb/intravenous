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

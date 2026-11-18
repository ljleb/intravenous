# Manual fallback before per-instance input toggles

_Historical, non-normative. Preserved on 2026-11-18 after the owner moved
fallback to the node instance and approved manual values for Coverage inputs._

## Node presentation and manual controls

Source: [Persistent manual controls](../node_presentation_and_manual_controls_direction.md#persistent-manual-controls).
That section now specifies the instance toggle and Coverage eligibility.

An eligible sequential input may have persistent project-owned manual-control
state.

The state contains at least a stored manual value plus the input's manual
participation policy. The intended behavior is:

```text
eligible sequential input
    has persistent manual-control value

no external source + manual mode enabled
    manual value participates

external source present + disconnected-only mode
    manual value remains visible/editable/persisted
    manual source does not participate

external source present + participate-with-connections mode
    manual source participates according to the input's defined
    composition/fan-in semantics
```

Connecting an external source does not erase the manual value. Disconnecting it
does not recreate a default from scratch. The stored value survives ordinary
connection edits and belongs in durable project state/JSONL once that persistence
surface is implemented.

The temporary override section began with:

> Only Sequential inputs receive tweakable manual controls.

The implicit default section began with:

> An eligible input's authored default is an implicit manual value until a user
> changes it.

## Automation and suspendable coverage

Source: [Absence and Sequential fallback](../automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback).
That section now specifies the same instance-level rule for sparse inputs.

_Settled by the repository owner on 2026-11-15. This replaces the earlier
Region/Coverage fallback and resolved-read proposals, preserved
[historically](./historical/automation_random_access_fallback_before_sequential_resolution_2026-11-15.md)._

Region and Coverage inputs preserve the sparsity and identity of their provided
Regions. `region()` / `regions()` exposes actual selected data; `coverage()`
is the positional union of those Regions. Overlaps and duplicate Regions
retain their independent contributions. Reading outside the provided ranges
is invalid. There is no manual knob, neutral substitution, fallback
configuration, `uncovered_value()` or fallback `read()` interface for these inputs.
An actual zero and the absence of a value remain distinguishable.

Sequential inputs provide a value at every position they process. Where upstream
coverage is absent, the input applies its declared fallback policy: its neutral
value or its persistent manual-control value. The ordinary Sequential accessor
returns the resolved stream; node authors do not implement fallback tests.
The default policy remains neutral, preserving existing input behavior. A
Sequential Sample input can opt into manual fallback. Existing non-Sample
neutral rules remain unchanged where no manual-control representation exists.

The module input paragraph ended with:

> Random Access module inputs preserve sparse Region/Coverage identity, and
> expose no manual fallback. The planner must not expose data outside its
> explicitly selected Regions and prepared windows.

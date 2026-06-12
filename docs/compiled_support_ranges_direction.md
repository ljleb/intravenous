# Compiled Support Ranges Direction

This note captures the current intended direction for compiled-lane support
semantics and how cache policy should consume them.

## Goal

Compiled lane outputs should not imply unbounded storage or unbounded
default-value bookkeeping.

Instead, compiled-capable lane nodes should explicitly describe where their
compiled output may be non-default.

## Lane-Node Surface

Compiled-capable lane nodes should define:

- `compiled_support_ranges(ctx)`

This should return sample-index ranges describing the support of the compiled
output.

The preferred range convention is half-open ranges:

- `[start_index, end_index)`

Examples:

- audio file lane: `[0, num_samples)`
- offset compiled lane: shift each compiled-input support range by the offset
- mix lane: union of compiled-input support ranges

## Input Visibility

Compiled support is semantic information and should be available to lanes that
need to derive behavior from compiled inputs.

This means:

- compiled lanes should be able to read compiled-input support ranges while
  implementing `compiled_support_ranges(ctx)`
- realtime lanes should also be able to observe compiled-input support during
  connection/update-time hooks if they want to size internal state from it

## Ownership

Support ranges should be derived and stored by `TimelineExecution`, not by
`LaneGraph`.

`LaneGraph` remains structural.

`TimelineExecution` should:

- derive compiled support ranges in dependency order
- store per-lane compiled support state
- expose that state to cache policy and input-change hooks

## Cache Relationship

Compiled cache policy should consume support, not define it.

That means:

- outside support, compiled outputs are the default value
- compiled cache entries only need to exist for regions intersecting support
- cache execution should be skipped entirely for requests outside support

This avoids storing endless "known default" chunks for lanes that have finite
or sparse support.

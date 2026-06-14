# Graph Input Repatching Direction

## Goal

`GraphInputLanes` should not think only in terms of vacant DSP graph inputs.

Instead, it should own authored desired state for logical knobs and concrete DSP
input ports, and it should re-complete realized DSP instance graphs when that
desired state changes.

## Builder ownership

- `IvModuleDefinitions` owns the source-derived canonical `GraphBuilder`.
- `IvModuleInstances` can request a fresh builder for a definition whenever an
  instance needs to be re-completed.
- Timeline or UI input-state changes should therefore rebuild from the
  remembered default builder, not from stale already-patched graph state.

## Synchronization boundary

Structural repatching should not happen while `TaskRunner` is mid-pass.

We want a synchronous pass-finished notification from `TaskRunner` so other app
modules can safely:

- rebuild realized DSP graphs
- swap timeline execution layouts
- apply other structural updates

without racing the current execution pass.

## Input-state model

There are two authored state layers.

### Logical sample knob state

- `overridden`
- `timeline_lane`

Logical knobs are initialized to their default values and can later be
reinitialized from defaults when needed.

### Concrete DSP sample input port state

- `default`
- `overridden`
- `logical_follow`
- `timeline_lane`
- `disconnected`

Defaults:

- `default` means:
  - vacant controllable input port -> `logical_follow`
  - already-connected DSP input port -> `disconnected`
- concrete sample inputs are initialized in `default` unless explicit state is
  authored later

`disconnected` is still meaningful for vacant inputs:

- vacant input + disconnected -> feed `Constant{default_value}`
- connected input + disconnected -> inject nothing extra and keep the existing
  DSP contributor

### Concrete DSP event input port state

- `default`
- `logical_follow`
- `timeline_lane`
- `disconnected`

Defaults:

- `default` means:
  - vacant controllable event input port -> `logical_follow`
  - already-connected DSP event input port -> `disconnected`

For event inputs there is no `overridden` state yet.

## Completion strategy

The graph builder should be allowed to accumulate multiple contributors into the
same DSP input port. The graph compiler already lowers sample fan-in through
generated `Sum<N>` nodes and event fan-in through event concatenation.

Therefore `GraphInputLanes` can complete a builder by connecting:

- existing DSP contributors already present in the builder
- optional logical-follow control source
- optional overridden control source
- optional timeline lane source

and let graph build lowering combine them.

## Rebuild policy

When desired input state changes:

1. `GraphInputLanes` updates its authored desired-state structures immediately.
2. If the change requires structural repatching, it marks the affected instance
   as pending rebuild.
3. On `TaskRunner` pass-finished, `GraphInputLanes` requests those instance
   rebuilds through bridges.
4. `IvModuleInstances` asks `IvModuleDefinitions` for fresh canonical builders,
   re-emits builder completion events, and the normal completion flow runs
   again.

Value-only changes should update the shared values immediately through
thread-safe shared state. Structural changes should rebuild at the safe pass
boundary.

Timeline lane structural changes should follow the same boundary. Lane batches
may be prepared eagerly inside `GraphInputLanes`, but they should only be
applied to `Timeline` and `TimelineExecution` between two `TaskRunner` passes.

## Current code direction

The useful remaining direction from this note is:

- keep builder completion derived from fresh canonical builders
- keep authored graph-input state in `GraphInputLanes`
- keep value-only updates out of the rebuild path
- keep both DSP repatching and timeline lane structural updates on the
  pass-finished boundary

UI shape is intentionally left out of scope here.

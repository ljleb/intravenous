# Graph Input Repatching Direction

## Goal

`GraphInputLanes` should stop thinking only in terms of vacant DSP graph inputs.

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

- `overridden`
- `logical_follow`
- `timeline_lane`
- `disconnected`

Defaults:

- vacant controllable input port -> `logical_follow`
- already-connected DSP input port -> `disconnected`

`disconnected` is still meaningful for vacant inputs:

- vacant input + disconnected -> feed `Constant{default_value}`
- connected input + disconnected -> inject nothing extra and keep the existing
  DSP contributor

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

Value-only changes should update the shared values immediately. Structural
changes should rebuild at the safe pass boundary.

## Scope of the current implementation pass

The first implementation pass does not need to redesign the VS Code UI.

It only needs to:

- add the pass-finished synchronization point
- expose builder logical inputs rather than only vacant inputs
- teach `GraphInputLanes` to own desired sample-input states
- rebuild realized instance graphs from remembered builders when required

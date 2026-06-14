# Graph I/O Repatching Direction

## Goal

`GraphInputLanes` should not think only in terms of vacant DSP graph inputs.

Instead, it should own authored desired state for logical DSP-facing inputs and
outputs, plus their timeline-lane exposure policy, and it should re-complete
realized DSP instance graphs when that desired state changes.

## Builder ownership

- `IvModuleDefinitions` owns the source-derived canonical `GraphBuilder`.
- `IvModuleInstances` can request a fresh builder for a definition whenever an
  instance needs to be re-completed.
- Timeline or UI I/O-state changes should therefore rebuild from the
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

## Output-state model

Outputs default to `disconnected`.

Unlike inputs, outputs do not need a separate `default` state because
`disconnected` already expresses the intended initial behavior: nothing is
exposed to the timeline unless the user explicitly asks for it.

There are again two authored state layers.

### Logical DSP output port state

For both sample and event outputs:

- `disconnected`
- `timeline_lane`

If a logical output is `timeline_lane`, a timeline-facing logical output lane
exists for that port. If it is `disconnected`, no such lane exists.

### Concrete DSP output port state

For both sample and event outputs:

- `disconnected`
- `logical`
- `timeline_lane`

Meaning:

- `disconnected`
  - the concrete DSP output contributes nowhere
- `logical`
  - the concrete DSP output contributes to the logical output aggregation for
    its logical port
- `timeline_lane`
  - the concrete DSP output gets its own dedicated timeline-facing lane

For logical output aggregation:

- sample outputs should rely on ordinary graph fan-in so the builder/compiler
  lowers multi-contributor aggregation through the existing `Sum<N>` path
- event outputs should rely on ordinary event fan-in / concatenation already
  used elsewhere by the graph builder and compiler

No special aggregate node needs to be introduced beyond the existing graph
lowering behavior.

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

The same applies on the output side.

`GraphInputLanes` should be able to complete a builder so that:

- concrete outputs in `logical` feed the logical output aggregation surface
- concrete outputs in `timeline_lane` feed their own dedicated timeline-facing
  output lane
- concrete outputs in `disconnected` feed neither

For logical outputs in `timeline_lane`, the aggregation surface exists only if
the authored logical state asks for it. `GraphInputLanes` does not need to keep
an aggregate node alive across rebuilds; rebuilding from the canonical builder
and letting normal lowering recreate the fan-in is sufficient.

## Rebuild policy

When desired I/O state changes:

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

For outputs, practically every authored state transition is structural:

- logical output `disconnected <-> timeline_lane`
- concrete output `disconnected <-> logical`
- concrete output `disconnected <-> timeline_lane`
- concrete output `logical <-> timeline_lane`

Those transitions should therefore rebuild on the pass-finished boundary too.

## Current code direction

The useful remaining direction from this note is:

- keep builder completion derived from fresh canonical builders
- keep authored graph I/O state in `GraphInputLanes`
- keep value-only updates out of the rebuild path
- keep both DSP repatching and timeline lane structural updates on the
  pass-finished boundary
- keep graph-output exposure opt-in, with outputs defaulting to
  `disconnected`
- keep logical output aggregation expressed through ordinary graph connectivity
  so existing `Sum<N>` / event fan-in lowering handles it

UI shape is intentionally left out of scope here.

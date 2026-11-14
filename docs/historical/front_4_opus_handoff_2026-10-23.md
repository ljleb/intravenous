# Front 4 Opus Handoff, 2026-10-23

_Status: historical, non-normative. This records the implementation handoff that
followed commit `a9cda499`. The active status is in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md)._

The handoff reported that the background side of Front 4 had landed:
`BackgroundEvaluationPlan` delivery classification,
`BackgroundStorageRealization`, transactions, `TickMaterializationSnapshot`
playback, neutral values, and generated-root overlay of
`TickSequentialBindingPlan` slots and Random Access views from
`TickInvocationCall`.

It divided the next implementation into the following intermediate steps.

## Reported step A: sample planning

`plan_sample_ports()` skipped a `SampleConnectionPlan` without a Tick-to-Sequential
channel and a `SampleProducerGroupPlan` without realtime connections. It planned a
mixed producer group from its realtime channels. A Tick output with only background
consumers received the ordinary disconnected-output sink used by persisted capture.
A background-fed Sequential input was then replaced by its
`TickSequentialBindingPlan` slot in the generated root. Mixed live/background channel
composition and Random Access inputs on Tick primitives remained unsupported.

The handoff said that the collaborator's run passed the existing suite, including
the two Front 2 fixtures blocked by background-only sample delivery. That statement
was not accompanied by a retained test transcript and must not be treated as current
verification.

## Reported step B: event planning

The handoff changed `inventory_event_connections()` so that one configured event
connection could become two `EventConnectionPlan` records, both with the same
`configured_connection_index`, when its targets used different delivery classes.
Realtime event planning skipped the background-only copy. A background-fed
Sequential event input received an invocation-supplied binding. Disconnected Random
Access event inputs remained unsupported.

The split was removed during the subsequent audit. `EventConnectionPlan` already
stores `targets`, `target_plans`, and one `EventDeliveryPlan::mechanism` for every
source/target delivery. Duplicating that record made the configured connection index
non-unique and forced downstream code to recover information that the original plan
already represented. Mixed event delivery therefore remains explicit Front 4 work.

## Invocation-supplied bindings

The handoff found that `overlay_tick_sequential_bindings()` treated a missing or short
`TickInvocationCall` span as a disconnected input. That behavior was not specified by
the design, duplicated the neutral fill performed by `TickInvocationWorkspace`, and
added a presence check plus a select per dynamic slot on every block.

The replacement representation added `invocation_supplied` to
`PrimitiveSampleInputBindingPlan` and `PrimitiveEventInputBindingPlan`. Such an input
has no ordinary realtime storage. The generated root copies its complete slot without
a per-node presence check. `validate_tick_runtime_plan()` validates the retained slot
ranges, and `TickInvocationWorkspace` produces complete spans. A disconnected
Sequential event input instead uses one compiler-emitted read-only empty sequence per
event type.

## Cross-kind transition friction

An invocation-supplied Sequential input has no bytes in realtime `NodeStorage`.
Consequently, the existing `RealtimePortStateTransitionPlan` cannot preserve its
history when a graph revision changes that input between background delivery and
realtime delivery. The predecessor background window is owned by
`TickInvocationWorkspace`, while the transition transfer operations currently address
only predecessor and successor `NodeStorage`.

The intended semantic rule remains that port history and latency are node-owned state
across a graph revision:

- background-fed to realtime-fed must seed successor history from the predecessor
  generation's playback window, not from data that might have been republished later;
- realtime-fed to background-fed must present inherited realtime history until that
  history expires; and
- background-fed on both sides inherits no realtime bytes because playback reads the
  selected data again for each invocation.

Implementing this rule requires an explicit executor/transition-preparation interface
for `TickInvocationWorkspace` history. Until that interface exists, both cross-kind
directions remain explicit compilation errors rather than silently substituting
default values.

## Deferred optimization

Running every contextually replayable Tick node only through background replay was
classified as an optimization and moved to Front 16. It depends on advance
materialization demand so that background execution produces the replayed values
before the realtime playhead needs them.

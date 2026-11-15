# Cyclic Automation, Event Projections, And Suspendable Coverage Propagation

_Status: accepted normative design direction (2026-11-15). The implementation in
`src/intravenous/basic_nodes/automation.h`, the event-port schema, and the
background scheduler have not yet been migrated to this contract. This document
records intended behavior, not a claim about the checked-in implementation._

This design extends [Coverage, Random Access, And Background Evaluation](./coverage_and_background_evaluation.md),
[Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md),
[Batched Node Callback Direction](./batched_node_callbacks_direction.md), and
[Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md).
It supersedes the value-blind, single-visit propagation restriction and the
all-circle linear automation behavior in the staged implementation. Earlier
coverage scheduling wording is preserved in
[historical/coverage_before_suspendable_propagation_2026-11-15.md](./historical/coverage_before_suspendable_propagation_2026-11-15.md).

## 1. Points describe outgoing segments on the circular domain

The `GlobalIndex` domain is circular. An automation curve is an ordered cyclic
sequence of control points, not a finite ordered list whose first and last
entries acquire special meaning. A point carries a `Sample` value and an
outgoing segment selection:

- **empty**: its segment up to the next point produces no output coverage;
- **hold**: the segment produces the point's value;
- **bezier**: the segment interpolates from this point's value to the next
  point's value, using automatic tangents and its tension setting.

The last point's next point is the first point. Segments are half-open along
forward circular index order: the current point's index belongs to its own
outgoing segment; the next point's index belongs to the next segment. An
`empty` point still retains a value needed as the preceding Bézier segment's
endpoint. It cannot be represented by `EmptyEvent`, which carries no value.

Zero points give empty output coverage. One `empty` point gives empty coverage;
one `hold` or `bezier` point gives a full-circle constant output at its own
value. Two or more points partition the circle into outgoing segments. The
union of nonempty (`hold`/`bezier`) segments is the curve's exact output
`IndexCoverage`. No separately authored support interval, special first/last
point roles, or implicit interpolation through an `empty` segment is needed.
The point source's finite storage `GlobalIndexRange`, including any
largest-gap choice for efficiently storing points, is independent of the
interpolator's output coverage and must never determine its active intervals.

For example, an opening `bezier` point and a later `empty` point create a
bounded active interval. Changing the latter to `hold` holds its value around
the complementary arc, with a permitted discontinuity at the opening point.
Changing it to `bezier` explicitly interpolates over that long arc instead.
The resulting long-period interpolation and float32 precision consequences
are authored choices, not artifacts of an implicit full-circle linear rule.

A dedicated `CurvePointEvent` carries the redundant fields useful for these
semantics, conceptually `Sample value`, outgoing `CurveInterpolation` and
`tension`. Point identity for editing is separate from the event payload.
Duplicate timestamps and a specific tension serialization convention require
an explicit implementation decision; they must not silently change the
half-open segment/coverage rules.

## 2. Automatic interpolation, not hand-authored handles

Use ideas from Blender's automatic and clamped handles rather than making
manual Bézier handles necessary. Derive endpoint derivatives from adjacent
point positions and values, respecting irregular index spacing. Prefer a
monotone/clamped automatic cubic for parameter automation so that interpolation
cannot overshoot the two endpoint values. A single segment tension controls
the interpolation family, including a value giving **exact linear**
interpolation. The precise tangent estimator, its behavior at adjacent
`hold`/`empty` segments, and the tension-to-shape mapping remain to be settled
as part of implementation; an illustrative smoothstep or Hermite formula is
not itself the normative formula. `empty` segments do not imply continuity
of tangents through the gap.

## 3. Event types and explicitly meaningful conversions

The general `Event`/`EventTypeId` system may contain application-specific
payload alternatives. The existence of an event type does **not** require
conversions to or from every other event type.

`EventConversionRegistry` is a directed graph of *explicitly defined* semantic
transformations, generally from information-rich representations toward
simpler projections. A tree is unnecessarily restrictive: a source may admit
several independent useful projections. A conversion that loses events by
filtering must document that property; it is not equivalent to a lossless
C++ subtype conversion. Never invent missing payload meaning merely to make
all event types connected. Absent conversion paths are normal.

Potential `CurvePointEvent` projections are:

- `SampleEvent`: the timestamp and stored value, discarding segment policy;
- `TriggerEvent`: a point occurrence, discarding value and segment policy;
- `BoundaryEvent`: the outgoing active/inactive state at that timestamp
  (`hold`/`bezier` versus `empty`), discarding value and tension. This may
  include repeated begin/end states at consecutive points. Computing a
  *minimal* active-region boundary sequence requires sequence-aware
  coalescing; a per-event conversion must not claim to do that by itself.

These are meaningful candidates, not mandatory conversion edges until their
semantics and consumers are established. No curve-to-MIDI or MIDI-to-curve
conversion is implied.

Event port schemas should be extensible to accept multiple event types, with
`std::variant`-style consumption without mandatory conversion to one type.
Initially, a producer's possible type set may connect directly to an accepting
input if it is a subset of that input's set. For such ports, implicit event
conversions remain disabled until an unambiguous general rule is chosen.
The automation point-source/interpolator pair may simply transport one
`CurvePointEvent` type and does not depend on solving variant conversion
planning first.

## 4. Uncovered automation uses the destination's manual value

`empty` outgoing segments are **uncovered**, not covered with a zero sample
and not an interpolation toward a fallback value. An eligible destination
Sequential input selects the automation result at positions where the
connected automation covers the position, and otherwise selects the input's
existing persisted/live manual knob value. This is a selection policy, not
additive fan-in and not the existing temporary manual-gesture override.
The manual value remains independently editable while automation is connected.
The ordinary Sequential `neutral_value` rule remains applicable to other
inputs; it is not silently redefined for every connection. Fallback occurs
only on **known uncovered** positions, never merely because a background
coverage query is still pending.

## 5. Keep forward and reverse coverage semantics; allow suspension

Retain `propagate_forward_coverage()` and `propagate_reverse_coverage()` and
their distinct meanings:

- **forward** discovers *exact* output coverage and sound may-change output
  regions following semantic or input changes;
- **reverse** discovers sufficient upstream input requirements for demanded
  covered outputs. It may over-request conservatively.

Allow either authored scalar callback to remain synchronous or instead return
a suspendable coroutine task (the concrete runtime task type/ABI remains an
implementation decision). Existing synchronous `_batch()` callbacks remain
supported; coroutine batch callbacks are not a prerequisite. The traits layer
normalizes supported authored shapes without forcing ordinary, value-blind
nodes to allocate coroutine frames.

A suspendable propagation callback may request input coverage metadata or
materialized input contents. The request is nonblocking. Independent requests
can be initiated together and awaited as a group, so the scheduler sees the
full currently known outstanding requirement set at a suspension. The
context/scheduler must distinguish **successful reads** (recorded semantic
invalidation dependencies) from **explicitly awaited unresolved requests**
(scheduling prerequisites). An attempted read must not automatically create a
blocking dependency on every earlier attempted read.

The node developer need not author numbered propagation stages. The scheduler
resumes the suspended operation after its declared prerequisites are ready;
coroutine frames may retain intermediate planning computations. An operation
cannot suspend indefinitely without naming a missing prerequisite. Demand-
driven materialization needed to discover forward coverage is permitted even
before the forward callback can publish that coverage.

Planning may partially resolve independent output-port coverage results and
make them available **within the private candidate**, while others remain
pending. The scheduler tracks requirement readiness at least per output port,
information kind (coverage versus contents), and needed region rather than
blocking unrelated work on an entire node. It detects cycles among actual
unresolved requirements, not merely mutual links between whole nodes.
Known-empty coverage and not-yet-known coverage are different states. The
scheduler may interleave forward propagation, reverse propagation, dependency
materialization, and callback resumption. Forward invalidation is not deleted
or inferred wholesale from reverse planning; reverse requirements are not
inferred wholesale from forward invalidation.

One background transaction still selects one coherent semantic/data version
and a fixed input-snapshot environment, and commits no incomplete results.
Successful partial planning may be visible to other **candidate** operations
but never to playback as a partial publication. Superseded tasks are cancelled
and destroyed with valid callback-context and JIT-code lifetimes. Retained
input views or configuration references cannot outlive their selected version.

## 6. Requested-only random-access inputs of `tock_coverage()`

**Normative invariant:** each `RegionInputConfig` or `CoverageInputConfig`
visible to `tock_coverage()` (or to one lane of `tock_coverage_batch()`) exposes
only the exact covered input positions **requested for that evaluation** by
reverse propagation. Its node-facing `coverage()`, Region/range enumeration,
readability, and event iteration must not expand to include other positions
merely because they exist in a published persisted root, an upstream output's
complete coverage, or a larger cached/transaction-local materialization.

Formally, the node-visible input positions are

```text
visible_input = explicitly selected covered logical input Regions
(no other available Regions)
```

The *requested* selection is the finalized evaluation requirement for that
input, not every exploratory read used by a suspended propagation callback to
decide those requirements. Selection respects the existing **whole logical
Region** contract: a demand intersecting a producer Region may retain that
complete Region as an explicitly required unit; it does not create a clipped
pseudo-Region or reinterpret persisted Region identities. A `RegionInputConfig`
still receives one contiguous Region; a `CoverageInputConfig` may receive a
selected collection of Regions, preserving overlap and multiplicity. The
scheduler must make the complete Region(s) it selected part of the **explicit
resolved request** *before* callback binding. No unselected Region is exposed
merely because it is available in storage. An authored reverse callback can
intentionally select the full finite point Region, in which case seeing it is
correct. Event iteration is limited to those selected logical Regions,
including valid zero-event regions. Read access to any unselected Region is
illegal even if its contents are already cached.

The callback's own requested output regions are selected according to the
same complete logical output-Region requirement; the requested positional
domain is intersected with exact output coverage when establishing demand. Multiple downstream roots may be unioned into one
explicit request before invocation. The invariant is **per finalized logical
request**, not per original UI query, and each `_batch()` lane has its own
request. Two executions with identical selected versions, semantic inputs,
requested input/output regions, and configuration see equivalent node-facing
inputs regardless of unrelated precomputed or stored positions. This forbids
accidental dependence on ambient materialization history.

This requested-only view restriction applies to **Tock evaluation**; it does
not prohibit forward/reverse planning from consulting additional, explicitly
requested discovery inputs. Those exploratory reads must not leak extra
coverage into the subsequent `tock_coverage()` input binding.

## 7. Background batching is a scheduling optimization

Keep coherent candidate publication, persisted-output completeness, sound
invalidation, and exact covered request semantics. Relax the old global
"forward once, reverse once, evaluate once" scheduling constraint for
**planning**: independently ready requirements can execute in batches; blocked
coroutines resume when their prerequisites become ready, possibly revisiting
a node within one transaction. No fixed global forward-then-reverse phase
ordering may prevent materializing discovery inputs. The existing ordinary
`tock_coverage()` and `tick_block()`/replay production paths remain separate
from planning; do not silently turn evaluation into coroutine planning or
relax audio-thread restrictions.

Logical input/output Region selections remain exact at the node-facing
binding even when reverse requirements are conservative, data storage is
shared, or physical records are larger. Planner batching and materialization
reuse are allowed to exceed a consumer's requested Region selection
internally, but never widen that consumer's visible Tock input
Region/Coverage view.

## 8. Implementation coordination

This is the intended destination, not a description of landed source behavior.
The staged `PointSource`/`LinearCurve` still use `SampleEvent` and full-circle
linear interpolation. `EventConversionRegistry` and event-port configuration
still have single-type assumptions; the current background propagation
callbacks remain value-blind and one-shot. Implement the full dependency-ready
feature across event declarations, trait/ABI bindings, scheduler, exact coverage,
manual fallback, and automation rather than adding local compatibility
workarounds that make those contracts appear implemented when they are not.

# Cyclic Automation, Event Projections, And Suspendable Coverage Propagation

_Status: accepted normative design direction (2026-11-15). The implementation in
`src/intravenous/basic_nodes/automation.h`, the event-port schema, and the
background scheduler have not yet been migrated to this contract. This document
records intended behavior, not a claim about the checked-in implementation._

This design follows [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md), which supersedes the
old whole-Region materialization assumptions and establishes demand-scoped
computed data. The [2026-11-17 recovered coverage contract](./tick_input_union_and_feedback_direction.md)
restores acyclic input-union coverage and limits full-coverage overrides to
feedback SCCs; the November 16 removal of input-union semantics was erroneous.
It also extends [Coverage, Random Access, And Background Evaluation](./coverage_and_background_evaluation.md),
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
Coincident points preserve an instantaneous jump in their input-event order.
The first point at a sample index supplies the incoming segment's endpoint;
the last point supplies the outgoing value, policy and tension. Intermediate
zero-length segments contain no sample positions. Equal-index ordering must be stable across reconstruction. The point source
uses persistent point IDs to break equal-index ties; generated events retain
that producer order without adding point IDs to `CurvePointEvent`. An
interpreter consumes equal-time events in their stable supplied order, including
after upstream event transformations. At the shared index itself, the last
point's outgoing policy determines the value or absence of coverage.

These equal-time rules and the tension convention below were accepted by the
owner on 2026-11-15. The earlier open questions are retained in
[historical documentation](./historical/automation_manual_fallback_before_random_access_policy_2026-11-15.md).

## 2. Automatic interpolation, not hand-authored handles

Use ideas from Blender's automatic and clamped handles rather than making
manual Bézier handles necessary. Derive endpoint derivatives from adjacent
point positions and values, respecting irregular index spacing. Prefer a
monotone/clamped automatic cubic for parameter automation so that interpolation
cannot overshoot the two endpoint values. A segment's tension is in **[0,1]**: 0 gives exact linear interpolation and
1 gives the clamped automatic curve. Blend between those curves for intermediate
values. Newly added Bézier points default to **1**. The precise tangent
estimator and its behavior at adjacent `hold`/`empty` segments remain
implementation decisions; an illustrative smoothstep or Hermite formula is
not itself the normative formula. `empty` segments do not imply continuity
of tangents through the gap.

### Vue point editing and source-driven snapping (owner refinement, 2026-11-16)

The backend segment and tension definitions above are unchanged. The Vue editor
exposes tension via a handle near each point. Click empty space to add a point
and drag a point to move it. Shift-click a point to delete it. The context
menu offers segment options and **Delete point**. See
[Vue Lane And Automation Interaction Direction](./vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).
That document makes snapping automation points to a **cover chosen from the
top-ruler header's connected event Coverage input** an explicit intended
feature. All event payload types can project their occurrence times to
`TriggerEvent`; the specific cover is chosen through the ruler dropdown.
The existing
editor's double-click-add/right-click-delete behavior is an earlier source
checkpoint, not the destination. This is UI intent and does not add DSP
interpolation work to the audio thread. A clip-local seconds/sample ruler,
nearest-project-sample quantization during **every drag update**, and small
filled point markers with larger invisible hit areas are further UI
requirements; see the same document's
[clip ruler](./vue_presentation_refinement_direction.md#automation-clip-ruler-and-sample-accurate-editing)
and [point marker](./vue_presentation_refinement_direction.md#small-filled-control-point-markers)
sections. The graph-selected snap grid is additional to mandatory sample
alignment, not its replacement.

`AutomationCurve` stores independent minimum and maximum settings alongside
the authored point list. Each setting consists of a finite value and an enabled
flag. `PointSource` limits the value in each `CurvePointEvent` to the enabled
bounds before the interpreter computes the curve. The stored points retain the
original values. Their event positions, outgoing segment policy, tension, and
sparse coverage retain the same rules. When enabled bounds meet, all emitted
point values equal that bound. When a bound edit crosses the enabled opposite
side, `SetBound` limits the edited value and preserves the opposite setting.
`AddPoint` and `MovePoint` limit the edited point to the enabled bounds and
preserve every other point. Existing out-of-range values survive project
load, bound changes, and changes to segment type or tension.
[Earlier display-only bounds](./historical/automation_display_only_bounds_2026-11-18.md)

The presentation uses the enabled bounds for its vertical axis. Point edits
never rescale it automatically. Both `nano` and `mini` show the curve and its
gaps read-only. Point and tension editing are available at `editable` zoom.
[Vue Presentation Refinement](./vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories)

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

**Accepted event-grid requirement (2026-11-16):** every event type has a
specific timestamp-only conversion to `TriggerEvent`. Event envelopes already
carry an occurrence position; the conversion keeps that sample index and
ignores the event's payload. The rule gives any event stream a way to supply
ruler/snap timestamps, **not** a universal conversion between event payload
types or a way to invent missing events. The earlier optional-curve-trigger
wording is preserved
[historically](./historical/vue_nested_port_clip_scale_and_ruler_input_before_2026-11-16.md#previous-optional-curve-to-trigger-conversion-wording).

The relevant `CurvePointEvent` projections are:

- `SampleEvent`: the timestamp and stored value, discarding segment policy;
- `TriggerEvent` (**now required for all event types**): a point occurrence,
  retaining only its timestamp and discarding value and segment policy;
- `BoundaryEvent`: the outgoing active/inactive state at that timestamp
  (`hold`/`bezier` versus `empty`), discarding value and tension. This may
  include repeated begin/end states at consecutive points. Computing a
  *minimal* active-region boundary sequence requires sequence-aware
  coalescing; a per-event conversion must not claim to do that by itself.

The `SampleEvent` and `BoundaryEvent` projections above remain candidates;
`TriggerEvent` is the accepted universal timestamp projection for every
event type. No curve-to-MIDI or MIDI-to-curve conversion is implied.

Event port schemas should be extensible to accept multiple event types, with
`std::variant`-style consumption without mandatory conversion to one type.
Initially, a producer's possible type set may connect directly to an accepting
input if it is a subset of that input's set. For such ports, implicit event
conversions remain disabled until an unambiguous general rule is chosen.
The automation point-source/interpolator pair may simply transport one
`CurvePointEvent` type and does not depend on solving variant conversion
planning first.

## 4. Absence and Sequential fallback

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

**Sequential inputs resolve absence; computed Tick values and output coverage
remain distinct.** Each Sequential input supplies a resolved stream: it combines
covered upstream contributions and uses its own neutral/manual fallback only
when the selected source coverage is known absent. The callback receives an
ordinary value for each executed position, with no optional sample type.
The input's fallback is not an additional source contribution.

A live Tick `SequentialOutputConfig` produces a value for each physical
execution position, including positions where its inputs used fallback.
Outside feedback SCCs, an `input_union` declaration derives output coverage
from input coverage, while `full` declares full coverage. The callback's use
of fallback does not fill output gaps. A downstream Sequential input applies
its own fallback in those gaps. A buffered recording has separately published
historical coverage and valid contents. Contextual replay still requires the
selected dependencies to be available or reproducible.

The [2026-11-17 correction](./tick_input_union_and_feedback_direction.md)
restores that distinction. Only the recursive live-coverage proof is retired:
Tick members of feedback SCCs have effective full live coverage and cannot be
contextually replayed through ordinary background Tick replay. The declarations
remain unchanged. Earlier full-only wording is preserved
[historically](./historical/tick_coverage_before_intent_recovery_2026-11-17/automation_and_suspendable_coverage_direction.md).

Automation is a Tock-produced **computed** Region/Coverage result, not a
realtime Tick node with special authored sparse output coverage. The outgoing
`empty`, `hold`, and `bezier` point segments determine its exact output
`IndexCoverage`; only the demanded positions in nonempty segments are
computed. The procedure may span very large intervals without allocating
buffers proportional to its declared coverage. A downstream Sequential input
handles `empty` gaps through its own fallback, while a Region or Coverage
input preserves them without substitution. A consumer requiring an entire
contiguous Region must use an explicit bounded materialization node.

The same node's values must be deterministic for the same point/recording
snapshot and requested positions, regardless of cache history or query
batching. Tock forward/reverse propagation discovers relevant point
information and prepares exact required windows before the evaluator runs.
For a Sequential consumer, values are prepared in a bounded rolling window
before the audio-thread Tick callback. Known-covered but unavailable data
are an underrun, not an absence/fallback.

Public module inputs retain Sequential, Region and Coverage access contracts.
A module's public Sequential input resolves fallback **before** internal
routing/conversions/fan-out; flattening does not move that boundary to leaf
inputs. The resolved internal Sequential stream is covered as a live signal.
Random Access module inputs preserve sparse Region/Coverage identity, and
expose no manual fallback. The planner must not expose data outside its
explicitly selected Regions and prepared windows. See
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

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

## 6. Tock input and output views

### 6.1 What is selected, and what may be read

`tock_coverage()` receives the **explicitly selected logical Region metadata**
for every `RegionInputConfig` and `CoverageInputConfig` (and corresponding
output). Logical Region selection is determined by semantic reverse planning,
not by what the cache happens to contain. A Region's full range, ordering and
identity are preserved even when its content is only partially materialized.
Duplicate/overlapping Regions remain distinct. Valid zero-event Regions remain
selected metadata even when they have no event payloads.

Node-facing content access is separate:

- A **windowed** input exposes only the prepared read windows deliberately
  selected for its demand. Those windows may be contiguous spans but need not
  cover the enclosing Region. Access outside them is invalid, even if a warm
  cache contains more data elsewhere in that Region.
- A **complete-buffer** input exposes the whole selected Region as one
  contiguous buffer, but only after the background planner has established
  the requirement and the executor has materialized/pinned all its contents.
  A computed producer cannot directly meet that requirement; use an explicit
  bounded materialization node.
- Output `region()`/`regions()` describe the same full logical Regions, while
  writable **windows** describe the requested covered positions the evaluator
  is responsible for filling. Do not allocate or fill an entire logical
  Region merely because one output window intersects it.

`IndexCoverage` is a positional set, not a logical Region collection. An
input's `coverage()` remains precisely the positional union of its **selected
logical Region descriptors**, not a statement that every position in those
Regions is readable or currently computed. The reverse planner's required
positions and the evaluator's prepared content windows are separate from the
selected logical union. They do not become another input-side
`required_coverage()` convenience accessor. The evaluator does need an
explicit way to enumerate its selected prepared readable/writable windows;
full logical Regions alone are insufficient to determine the evaluation work.

Discovery reads during suspendable forward/reverse planning never enlarge the
ordinary evaluator's readable selection. If evaluation depends on discovered
values, it must receive corresponding selected windows or explicitly versioned
planning results. The executor must not leak cached unrelated values to the
callback.

### 6.2 Examples that test the minimum interface

| Tock node | Logical information | Requested materialization |
| --- | --- | --- |
| Automatic Bézier curve | Point-event Regions, outgoing segment and exact curve Region/Coverage ranges | Requested playback/output window only; not the full segment |
| Pointwise gain/copy | Selected input logical Regions and their identities | Prepared matching source and output windows |
| Convolution | Selected Regions and input history footprint | Output window plus required halo input windows |
| Overlapping mixer | Multiple selected Regions, including duplicates | Each contribution's demanded window, with overlap preserved |
| Event-group transformer | Ordered event Region list, including empty groups | Complete selected event contents as required by event semantics |
| Whole-clip normalizer | Stable clip Region identity and whole-input statistic | Can explicitly demand full input data to determine gain; output may still be a small window |
| Full-buffer FFT | Stable Region identity and complete-buffer input access | Entire selected bounded buffer must be realized before the callback |

The exact callback accessor names for prepared content windows are to be
settled during implementation. Preserve the existing named `region()` and
`regions()` concepts for **logical selection** rather than quietly returning
clipped pseudo-Regions. Avoid `output.requested_coverage()` if it merely
restates the writable windows; do not erase the windows themselves from the
interface. The reverse propagation callback keeps its own
`output.required()` demand contract.

### 6.3 Request determinism and callback boundaries

The selected logical input Region list depends on the authored graph,
selected semantic/data snapshot, and explicit reverse requirements; the
selected prepared content windows also depend on the requested output
positions. Enlarging the requested window may legitimately enlarge the
writable output window and readable dependency windows without modifying
Region identities. A larger input dependency requirement need not force a
larger output computation window unless the node's actual algorithm requires
it. A complete-buffer consumer is an explicit demand for the whole selected
Region.

For example, if an input Region spans `[0,100)` but reverse planning needs
only `[40,42)`, `input.region()` still describes `[0,100)`, while the only
prepared readable content may be `[40,42)`. `input.coverage()` remains
`[0,100)` as the union of the logical selected Region. Materializing
`[200,300)` earlier does not add that data to either callback-visible
selection.

For fixed semantic/data versions and the same requested positions, computed
samples/events must be independent of cache warmth, request coalescing,
worker scheduling, and different legal ways of partitioning the windows.
No randomly grouped output window may silently change a normalizer's gain,
interpolation endpoints, seed or boundary semantics. Cancellation or failure
cannot expose an incomplete requested window. `TockState` remains purely
non-semantic acceleration.

### 6.4 Current implementation mismatch to resolve during migration

The source is not yet the intended API: `RandomAccessContinuousInputPort` and
`RandomAccessEventInputPort` in
`src/intravenous/node/coverage_port_context.h` expose `coverage()` through a
separate `coverage_value`, while `region_views()`/`regions()` expose independent
Region descriptors. `BackgroundEvaluationTransaction::Impl::binding_selection()`
selects input `binding_coverage` from reverse-required positions but selects
`binding_regions` by complete intersecting Regions.
`BackgroundEvaluationCallFrame::seal()` then binds both records to the same
input port; consequently **current `input.coverage()` can be narrower than the
union of readable `regions()`**. The Region read accessor can still read the
whole selected Region. Output `requested_coverage()` is likewise bound from
selected output `binding_coverage` and may be expanded during output storage
selection. The migration must make the authored accessor semantics unambiguous
and test them; do not claim that the checked-in source already guarantees the
new naming rule.

## 7. Background batching is a scheduling optimization

Keep coherent candidate publication, **requested-window** completeness, sound
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

### Feedback SCCs and recorded boundaries (2026-11-17 correction)

Tick nodes in a feedback SCC have effective full live coverage, including
members declared `input_union`. No independent full contribution or recursive
coverage fixed point is required. Preserve each declaration. Explicit semantic
SCC membership excludes ordinary background Tick replay even when every member
is intrinsically replayable. An initialized `.detach()` remains part of realtime
scheduling. Published recording boundaries can supply historical data without
replaying their feedback producer.

This override is local to feedback SCCs. Acyclic input-union nodes retain sparse
coverage. The earlier proof and the mistaken full-only replacement are preserved
in the [historical predecessor](./historical/output_ports_before_computed_buffered_2026-11-16.md)
and [November 16 wording](./historical/tick_coverage_before_intent_recovery_2026-11-17/automation_and_suspendable_coverage_direction.md).

### Playback preparation and missed background deadlines

Owner decisions, 2026-11-15: prepare a successor's first playback window before
activation while its predecessor continues playing. Adopt the graph and its
matching immutable data/materialization selection at a whole-block boundary.
Unchanged persisted output Regions and payload pages may be shared by both
versions; changed dependencies invalidate only the affected prepared results.

If an active graph later outruns background preparation, silence the affected
playback block, report an underrun, and continue at the next ready block. Do
not invoke the graph against unavailable covered data, wait on the audio
thread, or substitute neutral/manual fallback for that unavailability.

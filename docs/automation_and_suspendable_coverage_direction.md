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

Fallback is not another contribution in fan-in. It applies where no upstream
contribution covers the position, and neither changes upstream coverage nor
fabricates Regions. Live manual gestures and their explicit source override
remain separate from this absence policy. Region/Coverage inputs have no
manual-control interface.

Automation is independent of destination controls: `empty` segments produce no
coverage, `hold` segments produce constant values, and `bezier` segments
interpolate. The same automation output may feed destinations with different
Sequential fallback policies; Region/Coverage destinations preserve its gaps.

The scheduler must distinguish known upstream absence from missing requested
materialization. Sequential fallback applies only on **known absent** positions;
pending or missing covered data cannot be treated as a gap. Previously accepted
playback data remain in use until a coherent replacement is ready. At the same
semantic snapshot and request, incidental cache history cannot change input
values or the selected Region views.

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

A `tock_coverage()` invocation (and each `tock_coverage_batch()` lane)
receives **explicitly selected complete input Region views**, rather than the
union of anything already computed or cached. Its input `region()` (for
`RegionInputConfig`) or `regions()` (for `CoverageInputConfig`) is the readable
collection selected by reverse dependency planning and complete-Region
closure. Reading beyond that collection is invalid, even if backing storage
exists. A selected Region's full range is readable: a narrow requirement
intersecting a larger Region never turns the Region into a cropped pseudo-Region.
Overlapping or duplicate Regions remain separate, and zero-event event Regions
remain valid. Exploratory data reads made during propagation do not
silently enter the Tock invocation's selected input Regions.

An `IndexCoverage` is a positional set, **not** a Region collection. On the
intended Tock-facing API, `input.coverage()` (if exposed) is precisely the
positional **union of the Region views provided to that invocation**. It is a
convenience over their ranges, not a second, separately selected source of
truth. For example, provided Regions `[0,100)` and `[50,150)` retain two
independent values at index 75 while their coverage is `[0,150)`. The
reverse planner's potentially smaller or conservatively larger input
requirement is **not** this coverage and is not implicitly exposed as
`input.required_coverage()`.

The reverse propagation callback already knows the positions it required;
the ordinary Tock evaluator only needs the finalized readable input Regions.
If evaluation depends on a value obtained while planning, it must either
recompute the same pure determination from its selected inputs or consume an
explicit, versioned semantic result of planning; it must not take the
reverse requirement as a substitute for semantic input values.

An output's `region()` or `regions()` provides the complete writable Region
selection for the invocation. These ranges establish which output data must
be produced. The scheduler owns the original downstream demand, intersects it
with exact output coverage, and expands to complete output Regions as required
by the Region/Coverage production contract. `output.requested_coverage()` in
the existing API is a positional summary of selected output work, not a
source of unrelated content; its information can be derived from the selected
writable Region ranges once whole-Region closure is complete. The intended
minimal authored Tock interface therefore has no separate raw-request
accessor. Keep `region()`/`regions()` for writable data, and optionally
`coverage()` as a cheap positional union of exactly those writable Regions,
matching the input-side naming. The current output
`requested_coverage()` spelling can be retired or renamed during migration;
reverse propagation retains its own `output.required()` for demand planning.
Do not introduce `produced_coverage()` or an input-side
`required_coverage()` without a concrete case that cannot be expressed by
Region views and propagation.

### 6.2 Examples that test the minimum interface

| Eventual Tock node | Input-side information needed | Output-side information needed | What fails if that information is removed? |
| --- | --- | --- | --- |
| Constant or deterministic index-based generator | None | Writable `region()` or `regions()` and their ranges | Cannot know which finite output Regions to fill |
| Pointwise gain/copy | Input `region()` or `regions()` containing values | Output writable Regions | Cannot access input samples or distinguish separate Regions |
| Convolution / neighborhood filter | Input Regions including explicitly planned halo samples | Output writable Regions | Cannot evaluate at a boundary without the halo; input requirement itself need not be visible |
| Mixer with overlapping clips | Input `regions()` preserving Region identity and duplicate ranges | Output writable Regions | Positional `coverage()` alone loses contributions in overlap |
| Gap/mask-aware mapper | Input Regions; optional derived `coverage()` for set tests | Output writable Regions | Without the convenience union, it must compute the union itself; no expressive power is lost |
| Sparse point-event interpolator | Event `region()`/`regions()`, timestamps, payloads, and neighbor lookup | Output writable Regions | Cannot inspect control points outside the output window, even though the planner can select their finite Region |
| Selector whose input A chooses which part of B is needed | Selected A and B Region contents | Output writable Regions | Without suspendable reverse planning, the B requirement cannot be discovered; `input.required_coverage()` is not needed in Tock |
| Whole-clip normalizer | Full input Region defined by the clip's semantic boundary | Complete writable output Region | If planning supplies only a local sample window, output semantics change with demand; exposing a request accessor cannot fix incorrect selection |
| Event transformer preserving separate groups | Event `regions()` including Region membership/order | Writable event `regions()` | A coverage union alone discards group membership, multiplicity, and per-Region ordering |

These are tests of **information required by an evaluator**, not a claim that
all those nodes already exist. The first two columns follow the authored
`RegionInputConfig`/`CoverageInputConfig` and
`RegionOutputConfig`/`CoverageOutputConfig` distinctions: a singular Region
port uses `region()`, while a Coverage port uses `regions()`.

### 6.3 Request determinism and callback boundaries

What changes the Tock-visible records:

| Runtime change | `input.regions()` / derived `input.coverage()` | `output.regions()` / derived output coverage |
| --- | --- | --- |
| Different downstream demanded positions | May change **only because** a different set of complete input Regions is explicitly required | May change as a different set of complete output Regions is selected |
| Different forward-published Region ranges or existence coverage | May change when the reverse requirement now selects different input Regions | May change when the node's exact output Regions change |
| Changed point/sample/event values without a Region-range change | Region ranges may stay identical; readable contents/version change | Writable ranges may stay identical; recomputed results change |
| Conservative enlargement of an authored reverse requirement | May select more full input Regions; this must not change results on overlapping output positions | No automatic widening from the input storage cache |
| Warmed cache or unrelated materialization | **Must not change** selected Region descriptors or their union | **Must not change** the selected output work for the same demand/snapshot |
| Different compatible batching of demand roots | May change total explicitly selected Regions for the combined request | May change total selected output Regions; overlapping outputs remain semantically identical |

For example, if a reverse callback requests `[40,42)` from an input
whose selected source Region is `[0,100)`, the input `region()` grants access
to the **whole** `[0,100)` Region and the proposed `input.coverage()` returns
`[0,100)` as its union. The scheduler retains `[40,42)` internally as the
reverse requirement; it is not another Tock input accessor. If previous work
also happened to materialize `[200,300)`, that extra Region is not added to
the callback input.

Given an identical semantic/data snapshot, node configuration, and output
position, the produced value/event sequence must not depend on whether the
scheduler satisfied a requirement from cold computation, previous persisted
records, transaction-local materialization, or a different compatible batch.
An invocation's selected complete Regions must be determined from semantic
planning and declared output Region closure, not incidental cache contents.
Different output demands may select different sets of complete Regions, but
any overlapping produced positions must have the same semantic results.

This is stronger than merely repeating the same invocation and seeing the
same input spans. A node must not define normalization, interpolation, random
seeds, or boundary treatment from arbitrary demand grouping unless such
boundaries are stable parts of its authored semantics. `TockState` may
accelerate computation but cannot alter the results. Inputs are immutable
for the selected version and Region iteration order is deterministic.

The background scheduler may read/cache/materialize additional data to
satisfy independent consumers. Those extra data never become readable by a
Tock callback just because they happen to exist. A provided Region may
contain positions outside the smaller reverse-required set **because the
complete Region was explicitly selected**, and this is intentional.

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

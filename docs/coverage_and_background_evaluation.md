# Coverage, Random Access, And Background Evaluation

[DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md) defines the vocabulary used here.

> **2026-11-16 accepted design supersession:** [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md)
> governs the intended `SequentialOutputConfig` and
> `OutputRealization::{computed, buffered}` contracts, disjoint Tick/Tock output
> shapes, computed-vs-buffered input compatibility, demand-scoped Region content,
> partial buffered publication, cache policy, recording, and realtime feedback.
> Any older passage below about `TickOutputConfig`,
> `OutputRetention::{ephemeral,persisted}`, mandatory retention, whole-Region
> materialization, full-coverage publication, or feedback coverage proofs is
> **superseded**, not a competing requirement. Original wording is preserved in
> the [historical predecessor](./historical/output_ports_before_computed_buffered_2026-11-16.md). Source names and migration-status notes continue to describe
> the checked-in implementation, not the destination.

> **2026-11-15 accepted direction:**
> [Cyclic Automation, Event Projections, And Suspendable Coverage Propagation](./automation_and_suspendable_coverage_direction.md)
> replaces the previous value-blind, single-visit forward/reverse planning rule.
> Both propagation directions remain, but may suspend on explicitly requested
> input information. `tock_coverage()` receives **explicitly selected,
> complete input Regions**, never arbitrary cached Regions; the positional
> `coverage()` convenience is their union, not the narrow reverse requirement.
> The superseded propagation sections are retained
> [historically](./historical/coverage_before_suspendable_propagation_2026-11-15.md),
> and the superseded Tock binding wording is preserved
> [here](./historical/tock_binding_before_explicit_selection_2026-11-15.md).
> Checked-in source continues to describe the earlier execution model until
> the migration is implemented.

> **Circular-indexing and coverage-algebra change:**
> [Circular Global Timeline Indexing](./global_indexing.md) supersedes this
> document wherever it assumes a zero-origin linear `SampleIndex`, saturating
> arithmetic, begin/end index regions, raw global-index ordering, or uses the
> unqualified name `Coverage` for positional set algebra. That positional type is
> now `IndexCoverage` over `GlobalIndexRange`. The displaced semantics are preserved
> in [historical documentation](./historical/linear_global_index_semantics.md).

> **Random-access continuous representation boundary:**
> [Random-Access Port Data, Audio Value Types, And Input Contract Direction](./random_access_port_data_direction.md)
> defines the node-facing continuous and event Region/Coverage contracts. Their
> Regions can overlap or have exact duplicate ranges, and each Region retains
> independent value/event storage. Canonical persisted storage uses owning
> `PersistedSampleRegion` and `PersistedEventChunk` records.
> `ContinuousRegionStorageView` binds continuous records to node callbacks.

This document is the normative design for **coverage and random-access DSP semantics**,
their incremental background-evaluation model, and the executor-side
storage/publication rules needed to make data at global positions usable by both
random-access consumers and Sequential inputs during Tick execution.

The normalized callback vocabulary is planned to include both scalar and multi-node
batch operations:

```cpp
tick_block(...);
tick_block_batch(...);
skip_block(...);
skip_block_batch(...);

tock_coverage(...);
tock_coverage_batch(...);
propagate_forward_coverage(...);
propagate_forward_coverage_batch(...);
propagate_reverse_coverage(...);
propagate_reverse_coverage_batch(...);
```

The checked-in implementation currently exposes the scalar compiler anchors. The
batch context types, trait normalization, and GraphJit scheduling contract are the
planned direction specified in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md). A `_batch`
operation means **multiple compatible node instances**, not multiple regions of one
node's `IndexCoverage`. Every batch lane retains its own exact coverage/state/context, and a
one-lane batch is always legal.

Node authors choose one supported scalar-or-batch implementation shape for an
operation; the traits layer derives both normalized scalar and batch operations. The
background scheduler may then use ready-frontier batching without changing the
transaction's coverage semantics. Suspendable scalar forward/reverse callbacks
are permitted alongside synchronous callbacks; suspendable `_batch()` callbacks
are not required initially. Ready-frontier batching does not impose a single
planning visit per node.

The central rules are:

> Input access, output production, and output realization are separate authored
> properties. Input access is `SequentialInputConfig`, `RegionInputConfig` or
> `CoverageInputConfig`; output shape is `SequentialOutputConfig`,
> `RegionOutputConfig` or `CoverageOutputConfig`; output realization is
> `OutputRealization::computed` or `OutputRealization::buffered`. Tick primitives
> declare only Sequential outputs. Tock primitives declare only Region or
> Coverage outputs. Both callback domains use the same realization modes; a
> concrete primitive uses one primary callback domain.

> `computed` supplies demand-executable data without a proportional allocation
> obligation. `buffered` supplies an independently addressable data resource;
> a complete contiguous Region buffer is promised only to a compatible
> complete-buffer consumer, after explicit complete-Region preparation. Neither
> setting chooses a cache-retention policy. Recorded Tick Sequential/buffered
> data are part of the selected authoritative data snapshot.

> A Coverage output never feeds a Region input. Region inputs select one
> Region and can require windowed or complete-buffer access. A computed source
> cannot directly satisfy a complete-buffer request: an explicit bounded
> materialization node is required. Tock evaluation sees the exact logical
> Regions chosen by the planner and only their requested prepared content
> windows; cached unrelated data do not leak into node-visible inputs.

> Output coverage and logical Region range do not determine how many samples
> are computed. The background planner resolves exact existence, conservative
> changes and reverse dependencies; demand and declared buffer requirements
> determine necessary materialization. A buffered output may have known covered
> positions not yet computed. Complete published **requested windows**, not
> complete output coverage, are the publication requirement.

> Tock evaluation and coverage/dependency discovery run exclusively off the
> audio thread. Live Tick Sequential streams produce samples continuously;
> background-prepared Sequential windows must be ready before realtime use.
> Stateful `.detach()` feedback is scheduled in realtime, not replayed through
> an unresolved background cycle. A cache miss is never an audio-thread request
> to run Tock, allocate or block.

> A recorded Tick Sequential/buffered output is Coverage-shaped when read from
> its published historical buffer, not a Region output. Recording queue blocks
> become readable only after background publication; data required to reproduce
> a committed recorded snapshot must survive cache eviction. Graph configuration
> plus the selected input/recording snapshot determine background results.

> Tiling and event/channel conversions preserve per-channel producer
> capabilities; explicit nodes perform materialization or shape conversions.
> Forward and reverse propagation may suspend on selected data and must remain
> independent of cache contents. Published readers pin coherent versions.

For the complete connection matrix and realization details, see [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

## 1. Port model

The intended port configuration has `SequentialInputConfig`,
`RegionInputConfig`, and `CoverageInputConfig`. Tick primitives declare
`SequentialOutputConfig`; Tock primitives declare `RegionOutputConfig` or
`CoverageOutputConfig`. The shared realization property is
`OutputRealization::computed` or `OutputRealization::buffered` (not a
retention flag). Data/value types and port pacing remain independent. Source
migration and verification status are recorded in the
[implementation plan](./automation_and_background_migration_plan.md).

Input access describes the consumer's addressing requirements, not the
producer's execution domain. A `tick_block()` may read prepared Random Access
views; a Tock output can prepare a Sequential consumer's rolling window.
Region/Coverage inputs can require bounded window access or complete buffer
access. Connection compatibility follows
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md), including the absolute Coverage-to-Region prohibition.

The initial Tick API requires `InputContentAccess::complete_buffer` for every
Region/Coverage input, including on outputless sinks. The background executor
requests and prepares their complete selected logical Regions before activation.
Windowed Random Access inputs are initially available to Tock callbacks only.
Tick Sequential inputs retain bounded background playback preparation.

`IndexCoverage` remains positional existence; logical Region descriptors and
partially materialized readable windows are distinct objects. Realtime
Sequential input fallback applies only to known absence, never to an uncomputed
covered position. Coverage propagation and data production do not run on the
audio thread.

## 2. `IndexCoverage` is the positional coverage boundary

Every tock output has finite positional coverage: a canonical union of nonempty
circular `GlobalIndexRange` values. A continuous Region or Coverage output publishes
its logical Region-range list, and the runtime derives this positional union from that
list. `IndexCoverage` is set algebra over positions only. It carries no values and
does not replace the data-bearing Region views. There is no separate bounding hull or
implicit infinite default-valued domain.

For example:

```text
GlobalIndexRange{from_signed(0), 480000}
U GlobalIndexRange{from_signed(4800000), 480000}
```

may describe two audio clips separated by a large empty timeline gap. The gap
requires no persisted-data records, validity metadata, reverse demand, or Tock
work.

Outside coverage, the output is not requestable by node code. The executor and
node wrappers should assert this contract in debug builds. This is stronger than
saying that reads outside coverage return zero/no-events: lowering is allowed to
assume such reads never occur and optimize accordingly.

Disconnected random-access inputs have empty coverage.

A random-access input's **intrinsic available** coverage is the canonical union
of the mapped, available coverages of its connected sources, including tock
outputs, finalized published persisted tick outputs, and contextually replayable
tick outputs. `tick_block()` may inspect its selected prepared available view;
`tock_coverage()` inspects instead the complete input Region views
explicitly selected by reverse planning and Region closure. Those Region
views may contain more positions than the narrow reverse requirement, but
never include unrelated cached Regions. The
replayable `tick()` subset may itself have random-access inputs; those inputs must be
available through the immutable prepared view selected for the live/replay invocation.
An ordinary unretained, unreproducible tick stream has no arbitrary historical coverage.

Conceptually:

```cpp
auto input = ctx.input<"audio">();

for (auto region : input.coverage()) {
    // Random-access reads through this input stay inside coverage.
}
```

Internal lowering may retain per-connection coverage metadata in addition to the
node-facing aggregate union when conversion, fan-in, or dependency planning
needs to distinguish individual sources.

## 3. `IndexCoverage` helper

Node and runtime code should use one small helper value type rather than manually
maintaining vectors of regions.

Conceptually:

```cpp
class IndexCoverage {
public:
    using Range = GlobalIndexRange;

    IndexCoverage() = default;
    explicit IndexCoverage(Range);
    explicit IndexCoverage(std::span<Range const>);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(GlobalIndex) const noexcept;
    [[nodiscard]] bool contains(Range) const;
    [[nodiscard]] bool intersects(Range) const;

    void include(Range);
    void include(IndexCoverage const&);
    void exclude(Range);
    void exclude(IndexCoverage const&);

    [[nodiscard]] IndexCoverage intersection(IndexCoverage const&) const;
    [[nodiscard]] IndexCoverage difference(IndexCoverage const&) const;

    auto begin() const noexcept; // region iterator
    auto end() const noexcept;   // region iterator
};

IndexCoverage operator|(IndexCoverage, IndexCoverage const&);
IndexCoverage operator&(IndexCoverage const&, IndexCoverage const&);
IndexCoverage operator-(IndexCoverage const&, IndexCoverage const&);
```

The exact API may evolve, but the canonical representation invariant is fixed:

- every stored range has a nonzero extent;
- ranges are split at the signed-canonical ordering seam where necessary;
- ranges are stored in `SignedGlobalIndexOrder`;
- ranges never overlap; and
- adjacent ranges are coalesced in that explicit order.

The representation is an immutable path-copied balanced interval tree. Ordinary
copies share its root. A finite include or exclude operation replaces only paths to
the canonical ranges it touches, while ordered iteration uses a bounded
non-allocating iterator. This representation is part of the common `IndexCoverage`
type; persisted storage must not introduce a second positional-coverage type or
flatten coverage into a vector when publishing a snapshot.

Thus two adjacent ten-position ranges beginning at signed indices `0` and `10`
canonically become one twenty-position range. There is exactly one canonical range
sequence for a given positional set, including sets that cross the unsigned wrap.

`begin()` / `end()` iterate **regions**, not sample indices. Deliberately do not
provide a convenience bounding `extent()`, `first_index()`, or `last_index()` API
on this type: making sparse coverage explicit helps prevent accidental work over
long uncovered gaps.

`include()` / `exclude()` maintain canonical form. Construction from many ranges may
use a merge-oriented bulk path, while finite incremental mutations path-copy only the
balanced-tree paths and canonical ranges they touch.

Useful transformations such as shifting or finite support expansion may remain free
functions rather than growing the core container API. They must use modular
`GlobalIndex` operations and explicit finite extents; they must not reintroduce raw
integer ordering or saturating arithmetic.

The same type represents the exact positional union of output Regions, semantic
may-change positions, and reverse may-read requirements. A continuous output's
ordered logical Region list remains separate. Only the output's positional union is
required to be minimal: forward invalidation and reverse requirements are sound upper
bounds. Cache validity is deliberately coarser and is not represented by one
`IndexCoverage` bit per sample.

## 4. Coverage, persisted records, versions, and data

The executor distinguishes several concepts for persisted outputs:

- **coverage**: exact regions where the output exists and may legally be requested;
- **record range**: the explicit finite `GlobalIndexRange` authored by one immutable
  sample region or event chunk;
- **data**: retained sample values or the complete event set for that record range;
- **semantic version**: the dependency/configuration environment for which those
  values are meaningful; and
- **data version**: the immutable published persisted-data root against which those values
  were computed or read.

Coverage is exact and independent of record boundaries. Canonical persisted samples
are immutable contiguous `PersistedSampleRegion` records. Canonical persisted events
are immutable `PersistedEventChunk` records whose explicit range remains
authoritative even when the chunk contains zero events. Records are split only at the
signed-canonical ordering seam or where overwrite/erasure must preserve an unaffected
prefix or suffix. The whole-graph Tick block size does not partition persisted data.

One published root contains both sample and event output directories. A candidate
path-copies only the output-directory and per-output range-tree paths affected by its
mutations; unchanged outputs, records, and subtrees remain shared. Publication makes a coherent candidate visible as one immutable data version.
Completeness applies to the **requested windows promised by that version**;
other covered positions may remain uncomputed. Each published result tracks
which requested ranges are valid. This metadata is distinct from exact
`IndexCoverage`, logical Region identity and physical storage quanta.

RAM placement is intentionally not part of logical coverage or output realization.
A prepared window may use ordinary heap memory, stable arenas, mmap-backed files,
compressed backing, or another representation. Such choices must still satisfy
the audio-thread read requirements of the generated graph, for example by pinning or
prefaulting live regions where the backing mechanism can otherwise fault or block.
They do not reintroduce semantic holes into a published stored output.

## 5. Sparse logical requests and stored materialization

A sparse demand does **not** automatically select or compute a complete
logical Region. The planner preserves the full Region's identity, range and
order, and selects only the **needed content windows** for evaluation.
The requested windows may be widened by explicit dependencies or a
consumer's complete-buffer requirement, not by Region membership alone.
Clipping a materialization window does not create a new logical Region.

This rule applies equally to Tock/computed and Tock/buffered outputs. Computed
outputs may use bounded scratch storage regardless of full-circle coverage.
Buffered outputs can be materialized incrementally; published coverage may
include uncomputed positions. The executor records which demanded windows
are valid in a selected immutable version and never exposes a partial required
window. Buffered/cached values may be reclaimed if recoverable; recorded
inputs are authoritative snapshot data and must remain recoverable.

A Tick/Sequential/computed live stream exists for every executed position;
if it must supply historical background Random Access, contextual replay or
an explicit Sequential/buffered recording is required. The realtime audio
thread never computes missing background data. See
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

### Vue output queries are consumers of existing coverage

[Vue Output Observation And Query Direction](./vue_output_observation_direction.md)
specifies the intended `Observations` application client of this mechanism.
Only output-port values are in initial observation scope; Tock `Node::State`
and input-port value observations are deferred. A viewport request supplies
bounded output coverage/resolution to the background actor's existing batched
demand roots. An input required by reverse propagation is an internal dependency,
not a UI-observed input port.

A Tock/ephemeral query may compute sparse requested coverage and project the
transaction-local data into an owned bounded view result before that temporary
storage expires. It must not turn ephemeral output into persisted output.
A published Tock/persisted or Tick/persisted query pins an immutable stored
version and projects the requested subset; an unpublished Tock/persisted
successor is subject to ordinary candidate completion before publication.

Representative sparse waveform samples are an explicitly approximate display
operation. Exact min/max envelopes need interval data or suitable summaries;
exact event windows need event enumeration, not point sampling. Display gaps
and absence of published data accurately using the returned `IndexCoverage`
and version information. Do not infer that sparse positional requests imply
cheap producer computation in every graph. Playback-critical work takes
precedence over exploratory viewport coverage, and obsolete results may be
discarded without violating persisted-data transactions.

## 6. Dependency propagation remains independent of storage record boundaries

Storage record boundaries are representation details only. Semantic dependency
propagation remains expressed in `IndexCoverage` range space. Output existence coverage is
exact. Forward may-change regions and reverse may-read requirements may be conservative
supersets when an exact dependency footprint is unavailable or too expensive to derive.

If a mutation semantically changes:

```text
[523,530)
```

an exact node may report `[523,530)` downstream. A less precise node may report a
larger sound superset. If the reported range intersects a `tock/persisted` record,
the executor keeps the exact semantic range. It does **not** widen the already-reported
semantic may-change range to the whole record. The candidate store can split the
intersected record while structurally sharing every unaffected record and subtree:

```text
reported semantic may-change range
        |
        v
select exact covered recomputation demand
        |
        v
propagate_reverse_coverage(producer)
        |
        v
overwrite/erase exact candidate range
```

A `tock/ephemeral` request follows the same exact positional propagation but has no
canonical persisted candidate to update.

The important distinction is:

- forward semantic change may be conservatively widened by dependency semantics, but
  is never widened merely because it intersects a stored record;
- exact overwrite/erasure may split physical records without changing semantic
  coverage; and
- requested output demand remains exact at the storage boundary, while a node's
  reverse input requirement may still be a conservative upper bound.

## 7. Forward changes are independent of demand

Forward propagation is independent of random-access consumption and is not limited
to changes arriving through random-access input connections.

The executor has a closed set of **external invalidation roots**. Every external
semantic invalidation root for background coverage propagation belongs to one of
these classes:

1. **node-local semantic mutation**: an application/UI operation changes node
   state, configuration, or a resource according to that node type's own semantic
   rules;
2. **realtime-produced queue prefix**: one or more finite prefixes independently
   selected from producer queues change recording or Tick/persisted outputs at their
   recorded global positions. Queue insertion itself is not persisted-data
   publication;
3. **graph semantic configuration change**: node creation/removal/replacement,
   random-access input connection-set changes, or another graph/configuration change that
   changes background-evaluation dependencies or implementation semantics; or
4. **project sample-rate change**: background-computed output semantics are reevaluated
   under a new sample rate.

A random-access input changing because an upstream output changed is **not** another
root class. It is the ordinary continuation of the same forward batch through the
connection into that random-access input. Likewise, creation of a new computed node is handled as a
graph semantic configuration change whose old output coverage is empty.

Executable regeneration by itself is not an invalidation root. A compatible new
`CompiledGraph` generation rebinds stable persisted-output state; only a semantic
change exposed while reconciling the generation enters one of the root classes
above.

A node may therefore run its forward-coverage logic with **zero changed random-access
input regions** because local semantic configuration changed. The callback/context
must distinguish such a local-state-triggered update from the absence of work.

A forward update may change three related things for each continuous output
participating in background coverage propagation:

1. the output's new exact ordered logical Region-range list;
2. the exact positional `IndexCoverage` union derived from that list; and
3. a sound positional superset inside the old/new union containing every value that
   may have changed.

The executor owns the previous Region-range list, so node code publishes the new list
as a whole value. The executor derives both positional unions and then derives:

```text
added   = new_coverage - old_coverage
removed = old_coverage - new_coverage
changed = reported_value_changes | added | removed
```

Added or removed positional coverage is itself semantic change and propagates
downstream. A Region-list change whose positional union remains equal is also semantic
change; it invalidates the ranges of the affected old and new Regions even though
`added` and `removed` are empty.
For `tock/persisted`, every exact candidate range intersecting `changed`, added or
removed coverage, or another representation-invalidating change becomes stale in the
candidate. The reported `changed` set continues downstream unchanged by physical
record boundaries. It need not be minimal, but it must contain every actually
affected output position.

Forward propagation does not itself perform ordinary `tock_coverage()` value
production. It produces exact coverage and sound may-change regions, possibly
after requesting input materialization through the shared dependency scheduler.
Those ranges become internal candidate-completion demand. Reverse propagation
and necessary discovery materialization may interleave with pending forward
planning, but forward invalidation and reverse requirements retain distinct
semantics and all publication remains coherent.

## 8. `propagate_forward_coverage()`

The forward callback determines each computed output's **exact** logical Region
list or `IndexCoverage`, and a sound may-change superset containing every
position whose value or existence may have changed. Local semantic mutations
are roots even when no random-access input's coverage has changed. The
executor also adds the exact differences between old/new coverage and affected
Region-list shapes to downstream invalidation. Physical storage boundaries do
not widen the reported semantic change set.

The callback can be the existing synchronous authored form or a suspendable
coroutine form under the same name. Ordinary nodes can compute from configuration,
input coverage and structural metadata with no suspension. A node whose output
existence or affected range genuinely depends on input event/sample contents may
request those contents and suspend until a coherent selected-version view is
available. Conservative **change** regions are allowed; falsely publishing
empty or over-approximated **existence** coverage because inputs are pending is
not allowed. Exact output Region-list identity continues to matter even if the
positional `IndexCoverage` union is unchanged.

For example, the automation interpolator cannot determine the union of active
`hold`/`bezier` segments from the point input's positional coverage alone. Its
forward coroutine must be able to request the finite point-event Region, inspect
outgoing types, then publish exact active output coverage. This does not make
`LinearCurve`'s staged full-circle coverage an accepted fallback.

A replayable Tick node with mechanically pointwise dependencies may continue
using compiler-generated propagation. Its declared `TickReplayMode` selects the
union of its input coverages or the whole circle as output coverage; its
Sequential inputs resolve absent upstream positions either way (see
[absence and Sequential fallback](./automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback)).
More complex replayable Tick nodes use
these same authored propagation semantics without changing their Tick evaluation
into Tock evaluation.

## 9. `propagate_reverse_coverage()`

The reverse callback determines a sound set of upstream **covered input
requirements** sufficient to compute the demanded covered output ranges.
Requirements can be conservative, including complete finite point Regions
when needed to find sparse neighbors. The selected published persisted-data
version may terminate an upstream requirement only where its data are valid
for the candidate; stale retained data cannot satisfy a new semantic version.
A callback without upstream inputs needs no reverse implementation, and
mechanically known conservative requirements may still be compiler-generated.

The callback can be synchronous or a suspendable coroutine under the existing
name. It may request contents or coverage of one input to discover which
regions of other inputs are needed. Independent requests can be issued
concurrently and awaited as one group; dependencies need not be artificially
serialized in the order of source code. A missing read does not implicitly
make every earlier attempted read a blocking requirement. Successful reads
must nonetheless be tracked as semantic dependencies for invalidation.

Reverse propagation computes required inputs; it does not derive forward
coverage or replace forward semantic invalidation. Its resolved input
requirements are intersected with exact input availability before evaluation.

## 10. Tock coverage evaluation callbacks

`tock_coverage()` and `tock_coverage_batch()` evaluate **only the exact
prepared demanded covered output windows**, widened when dependencies or
complete-buffer input requirements make that necessary. A callback still
receives the full metadata/identity of each explicitly selected logical
Region. The selected materialized content may be a strict subrange of that
Region; a clipped content window is not a clipped logical Region.

For `RegionInputConfig` and `CoverageInputConfig`, node-visible Region
collections are determined by the planner from the declared request and
reverse requirements, not from incidental cache contents. `coverage()` is
the union of the selected logical Regions, not a claim that all their
contents have been computed. Explicit prepared windows are the only
readable portions for a windowed input; a complete-buffer input is bound
only after its whole selected Region is materialized and pinned. Duplicate
and overlapping Regions remain distinct.

Input/window selection and computed output values are deterministic for a
fixed semantic/data snapshot and request, independent of cache history,
work batching or hidden discovery reads. Selected prepared values must be
complete for each requested sample/channel; events must respect the
corresponding ordering and capacity rules. Failure or cancellation does not
publish incomplete windows. Tock-owned `State` remains non-semantic
acceleration; neither Tock nor dependency propagation executes on the
audio thread.

Detailed semantics: [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

## 11. Coalesced demand and coherent candidate completion

A transaction collects external random-access fetches, future sequential
playback materialization demands, selected fixed producer-queue prefixes, and
internal stale/absent `tock/persisted` completion requirements. Coalesce ready
compatible requirements without changing their exact covered semantics. Pure
reads of compatible published persisted data may stop without invoking Tock.

When a forward or reverse planner needs unmaterialized data, it registers an
explicit prerequisite that the scheduler can satisfy through reverse
requirements and background value production. This can happen before forward
coverage is completely known. Dependency resolution, forward invalidation,
reverse planning and required materialization may interleave; the prior fixed
one-forward-pass/one-reverse-pass ordering is not normative. Separate output
ports, information kinds (coverage or contents) and required positions may
become internally ready independently, avoiding artificial whole-node cycles.

No unresolved true requirement cycle is permitted. Readiness distinguishes
known-empty coverage from coverage not yet discovered. The scheduler does not
spin on an unresolved callback with no concrete pending requirement, and
superseded work cannot publish. A candidate is committed only when every
required persisted result is complete for the selected semantic/data version;
unrelated retained records and subtrees remain shared as before. Realtime Tick
execution never blocks waiting for new background discovery.

The ordinary `tock_coverage()` and replay evaluation operations are invoked
with their selected prepared requests. The scheduler may preserve efficient
single-invocation/coalesced evaluation when dependencies allow it. A hard
single-visit rule does not constrain **planning**, and scratch/storage decisions
must not leak unrelated cached or exploratory input Regions into evaluation.

## 12. Readiness-driven propagation and batching

Forward propagation still sends changed coverage and may-change information
through fan-out; reverse propagation still sends required covered positions
upstream until valid persisted boundaries or source nodes satisfy them. Both
operations use one selected versioned transaction environment, including the
selected immutable data root and fixed queue prefixes.

The scheduler tracks outstanding information requests and already-resolved
semantic dependencies. Several independent inputs may be requested together,
ready work across compatible node instances may be batched, and a suspended
scalar planning coroutine resumes only when its named prerequisites progress.
An input-dependent second request can emerge after a first request completes.
This sacrifices the historical *one propagation visit per node per transaction*
rule, not exact coverage or coherent publication. Existing synchronous
`propagate_forward_coverage_batch()` and
`propagate_reverse_coverage_batch()` remain legal fast paths; coroutine batch
planning is not required for the initial extension.

Publication and cancellation remain transactional. Resumable callback contexts,
selected input views, node configuration and JIT code must have lifetimes
sufficient for suspension, and no partially resolved candidate is exposed as a
committed data version. The previous value-blind, single-phase contract is
preserved in [historical documentation](./historical/coverage_before_suspendable_propagation_2026-11-15.md).

## 13. Authored port schema, realization, and replayability

The authored port schema separates input access from output shape:

```cpp
SequentialInputConfig<Types...>
RegionInputConfig<Types...>
CoverageInputConfig<Types...>

SequentialOutputConfig<Types...>
RegionOutputConfig<Types...>
CoverageOutputConfig<Types...>

enum class OutputRealization { computed, buffered };
```

Tick primitives declare only `SequentialOutputConfig`; Tock primitives declare
only `RegionOutputConfig` or `CoverageOutputConfig`. Both use
`OutputRealization::{computed, buffered}`. One concrete primitive has one primary
callback domain. Input access, output shape and output realization remain
distinct static properties; the callback domain constrains output shape.
Sample/event data properties remain separate. One input
capability determines whether a Region/Coverage consumer accepts prepared
windowed content or requires complete contiguous buffers. Computed outputs
do not directly satisfy the latter. Output realization **does not** prescribe
cache lifetime. See [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md) for the valid callback/shape combinations and
connection rules.

`TickOutputConfig`, `OutputRetention::ephemeral` and
`OutputRetention::persisted` are historical implementation identifiers.
The old retention contract and its complete-publication obligations are
preserved in the [historical predecessor](./historical/output_ports_before_computed_buffered_2026-11-16.md).

### Replayable node trait

Intrinsic replay eligibility remains separate from port output shape and
realization. Replay is permitted only for deterministic, side-effect-free Tick
computations with no stateful Sequential history/latency and only when
contextual dependencies can be obtained from the same immutable snapshot.
The former `TickReplayMode::{disabled,input_union,full}` coverage coupling is
historical. Intrinsic replay permission is independent of output realization;
`TickReplayMode::{disabled,full}` retains that permission in the current source
migration. A replayable node's own outputs must be computed Sequential outputs.
It may consume published buffered data from other nodes. Live Tick/Sequential output
existence is continuous without authored sparse replay coverage.
Stateful `.detach()` delays make unresolved feedback SCCs ineligible for
background replay. Port declaration validation still has to ensure that
replay cannot write into or mutate buffered resources as a side effect.


Event replay, if enabled, must additionally preserve deterministic timestamp/order
and declared capacity; do not silently infer event replay from the sample rule.

Existing node traits already generate the `tick_block()` wrapper for `tick()`;
GraphJit already imports its LLVM definition. Replay uses **that** imported
wrapper in the background evaluation DAG, allowing normal O3 inlining, fusion,
value specialization, and possible SIMD. It does not redefine `tick()` for ordinary
nodes or introduce a second implementation of the DSP algorithm. GraphJit generates
identity-time forward/reverse propagation for mechanically pointwise dependencies.
When a replayable Tick node has a non-pointwise Random Access dependency, such as a
source addressed by a `FractionalGlobalIndex` coordinate stream, its background dependency
mapping is supplied through the same forward/reverse propagation contracts used by
background planning. Conservative whole-input/whole-output dependency bounds remain legal where
sound; suspendable propagation may instead read selected input data to discover
tighter bounds or exact output existence when required.

The trait gives **intrinsic eligibility** only. An output is contextually
replayable for requested coverage only if all of its required upstream inputs are
reproducible or available in finalized published data for the selected semantic
version. A pure gain node fed by an unrecorded microphone is not historically
replayable; the same gain fed by recorded data can be. A retained published tick
output terminates traversal even if its original producer cannot replay. Replay
uses isolated invocation-local scratch, immutable configuration and a canonical
block-position/alignment contract; it cannot mutate shared live execution state.

Random Access replay dependencies are an availability requirement, not an alternate
execution mode inside `tick()`. Before a background replay invocation, the background
transaction resolves the selected immutable versions, runs any required Tock/replay
work, and installs the planned Region/Coverage views. During ordinary audio-thread
Tick execution the same rule is stricter: every Random Access input is already bound
to its callback-pinned persisted/materialized view before `tick_block()` begins. Tick
code never invokes Tock, performs dependency discovery, waits for materialization, or
extends its Random Access coverage in response to values it reads.

This replay mechanism is also the current answer for procedural values that can be
cheaply recomputed at arbitrary positions: they remain eligible constrained Tick
nodes rather than introducing a fourth Region/Coverage/Sequential access form. A
future explicit `tack()`/`tack_block()` callback family has been considered as a way
to make replayability first-class while returning `tick()` to foreground-only
semantics, but it is deliberately **not** current direction.

## 14. Canonical persisted data uses explicit sample regions and event chunks

All persisted outputs use one canonical `PersistedDataStore` regardless of whether
their producer is Tick or Tock. The store publishes one coherent immutable root for
sample and event data, but it does not force both data kinds into one physical record
shape.

Persisted samples are immutable contiguous `PersistedSampleRegion` records:

```text
PersistedSampleRegion
    output handle
    GlobalIndexRange
    ChannelLayout
    contiguous sample values for exactly that range
```

Persisted events are immutable `PersistedEventChunk` records:

```text
PersistedEventChunk
    output handle
    authoritative GlobalIndexRange
    EventTypeId
    absolute GlobalIndex event coordinates ordered forward from range begin,
    possibly empty
```

Neither record width nor record boundaries are derived from the whole-graph root block
size. Changing the realtime block size therefore does not repartition retained data.
Sparse sample gaps allocate no neutral-filled storage, and consumers that need a
contiguous sample span can use one sample region directly.

Each output is resolved off realtime to a compact store-local handle. An immutable
path-copy output directory selects that handle's state; a path-copy balanced range
tree selects the containing/intersecting records in signed `GlobalIndex` order.
Circular input ranges are split only at the signed-order seam internal to the store.
Readers never perform raw page-number arithmetic or treat signed order as universal
timeline order.

The store keeps logical Region lists separately from its physical range index. A
`replace_sample_regions()` or `replace_event_regions()` batch preserves logical order,
overlap, and exact duplicates. Its replacement domain cannot clip an old logical
Region. The physical range index may contain signed-seam slices and may select one
record for point lookup; it is not the node-facing Region list.

An ordinary Tick-recording overwrite or explicit void remains positional. It removes
only its intersecting range, can split a crossing physical record, and changes the
candidate's `IndexCoverage` by the exact overwritten or erased range. One transaction
publishes the sample/event successor root atomically. Older roots remain readable
while pinned and are reclaimed only off realtime.

Tick/persisted retention begins when a completed Tick invocation leaves its output
`written` or `voided`. Its addressed callback range enters this store through the
recording queue; no implicit recorder or Tock callback is required. Tock/persisted
output enters the same candidate/publication path after its selected coverage is
materialized. Production mode changes how records are filled, not the retained-data
read path.

Tick-capture queue blocks remain pre-publication background inputs, not a second
persisted representation. Consumed blocks become reclaimable only after the
transaction that consumed them commits, subject to callback-boundary lifetime rules.

## 15. Persisted event chunks, fan-in order, and random-access iteration

An event chunk's explicit range is authoritative. A published empty event chunk means
the **complete event set for every position in that range is known to be empty**; it is
not equivalent to missing capture or unavailable coverage.

`EventOutputProperties::max_events_per_index` is density/capacity metadata, not
a literal per-timestamp quota. Without a finite output count, a selected range
uses:

```text
required_event_entries = ceil(max_events_per_index * range.extent)
```

_Accepted clarification, 2026-11-13:_ a sparse producer can declare a finite
`max_event_count` over its complete output coverage through
`ctx.output<"name">().max_event_count()` in `constrain_ports()`. Background
storage uses that count instead of estimating the clip's event count from its
sample duration. The bound counts events across all logical Regions; it does
not merge or identify those Regions. Multiple declared upper bounds use the
smallest bound. The earlier density-only wording is
[preserved here](historical/event_capacity_before_finite_point_counts_2026-11-13.md).

Background event storage may grow as events are written. It must still reject a
write that exceeds the selected bound, and failed evaluation must not publish
partial data. A long, sparse event Region must not reserve one entry per sample
before its callback runs.

Sequential playback and background replay allocate their event buffers before
execution. Finite source counts survive conversion and fan-in. Sources without
a finite count retain their density bound for the selected window. Fan-in must
reserve enough for every source contribution, including bursts, without
allocating in Tick. Ordinary authored realtime event outputs retain their
existing rate-based callback-storage contract.

Background evaluation may dynamically commit persisted event data capacity off the
realtime thread. Tick-produced persisted events and explicit recording use
producer-specific reserve blocks supplied ahead of demand by the async
capacity manager; the realtime path consumes only already-assigned blocks and performs
no request-sized allocation. A future recent-data overlay could use recent queued
event payloads in front of the published event-chunk root, but the baseline Random
Access path does not.

Event order is semantic and deterministic. Within one producer and one selected
finite range, events are emitted in nondecreasing forward order from that range's
explicit origin. Fan-in uses a stable tie break:

```text
1. forward offset from the selected range origin
2. stable semantic source/connection index
3. producer-local event order
```

Chunk evaluation order, worker scheduling, cache/store layout, and request order
must never change equal-timestamp ordering.

An Event Region input exposes one `EventRegion`. An Event Coverage input exposes an
ordered `EventCoverage` list. Each entry keeps its own event sequence; overlapping or
identical Region ranges do not merge their payloads. A Region iterator clips reads to
that Region and never exposes events outside the requested subrange.

A Sequential event consumer receives one premerged immutable timeline. Background
publication or materialization builds that timeline in advance. Tick playback only
binary-searches and visits the selected window; it does not allocate or sort. Events
from every logical Region remain present. Equal-time ordering uses semantic source
order for fan-in, then Region order and producer-local order.

## 16. Persisted output storage, stable output identity, and `NodeStorage`

Each executable realization still has one canonical fixed-layout `NodeStorage`, but
dynamically sized persisted-output data is **not** part of that fixed layout and
should not be owned merely by one JIT generation when the output has stable project
identity.

`NodeStorage` is the packed fixed-layout **audio-thread** realization. It contains
realtime storage whose shape is known when the `CompiledGraph` is built, including:

- sequential `State`;
- compiler-owned realtime persistent regions; and
- Tick carry/history/feedback storage selected for persistent placement.

The `State` of a Tock primitive is deliberately outside realtime `NodeStorage`.
`BackgroundExecutor` owns it as per-node background acceleration state, and it may
contain dynamically allocated structures. Reusable background workspaces are
likewise background sidecars rather than `NodeStorage` merely because they happen
to be bounded.

Tock-owned `State` is not an authoritative persisted-output store and is not
shared with `tick_block()` or propagation callbacks. A node's observable behavior
must remain correct if it is discarded or independently initialized.

`BackgroundExecutor` instead owns stable persisted-output storage plus
per-generation port mappings. Conceptually:

```cpp
struct StablePersistedOutputId {
    StableConcreteNodeId node;
    StableOutputPortId output;
};

struct StoredPersistedEntry {
    StablePersistedOutputId id;
    OutputProductionConfig production;
    OutputRetention retention = OutputRetention::persisted;
    // Versioned coverage/root state and stable data storage.
};
```

Ownership rules:

- every persisted output binds to one canonical persisted-data store entry keyed by
  stable output identity when that identity exists, regardless of Tick/Tock production;
- compatible executable generations rebind to that stable data-store entry rather
  than copying retained data;
- anonymous persisted outputs may use generation-local data-store identity;
- ephemeral outputs own no persisted-data entry to migrate or rebind; and
- provisioned blocks are runtime pre-publication/background inputs and are
  not a second published representation.

The canonical store has one immutable published snapshot root spanning all sample
and event entries selected by a transaction. Sample regions and event chunks have
separate typed payloads, but they share the same authoritative semantic/data version
and publication operation. There is no independent sample authority, event authority,
or generation-local persisted database. A candidate is mutable
only while private to its transaction; readers can acquire only an immutable published
snapshot.

Reader pinning must be suitable for the audio-thread boundary. Acquiring or releasing
a Tick reader pin must not allocate, block, take an unbounded lock, or synchronously
destroy the last owner of a retired snapshot on the audio thread. Publication and
reclamation therefore use a callback-boundary/epoch/RCU-style handoff, or an
equivalent design that defers destruction to a non-audio thread. A plain
reference-counted pointer whose final release can reclaim storage in `tick_block()`
does not satisfy this contract.

Persisted-storage reuse is normally **rebinding**, not copying. Output directories,
range trees, file/mmap-backed roots if ever used, and immutable snapshots may
remain owned by the executor while old/new executable generations refer to appropriate
versions. Disk backing is not implied by `persisted`; it would merely be one backend
of the same persisted-data abstraction.

The currently specified storage forms are therefore:

```text
current Tick representation
canonical persisted-data store for Tick/persisted and Tock/persisted outputs
transaction-local addressable materialization for background ephemeral Random Access
materialized sequential/addressable window for ephemeral Tick-time consumption
producer-specific reserve/pending-queue SPSC transports for persistence staging and explicit recording
```

These queues are block-backed and provisioned independently of background-evaluation
progress. Tick/persisted production uses them to move finalized Tick values toward the
canonical persisted-data store without requiring a Tock callback. When layout permits, a queue
block's payload storage may also be the current Tick data read by same-Tick
Sequential consumers; this is storage coalescing of capabilities, not a second
persistence format.

## 17. Background evaluation transaction workspace

Coverage planning and background evaluation need request-sized temporary storage that is not
necessarily bounded at graph compile time. The runtime decomposition is fixed as
follows:

- `BackgroundCoverageState` (name may follow local naming conventions) owns the
  committed long-lived semantic coverage baseline for one bound generation;
- the standalone `BackgroundPropagationWorkspace` owns reusable F/R accumulator
  storage and builds the coverage portions of `BackgroundEvaluationCall`; it has no
  dependency on `BackgroundExecutor`, persisted data or materialized storage;
- `PreparedCoveragePropagation` owns candidate coverage and exposes immutable exact
  changes/requirements plus the per-node activity selected by successful F/R,
  without mutating the committed baseline;
- `BackgroundEvaluationTransaction` owns one pinned base snapshot, the prepared
  propagation result, a private persisted-data candidate, transaction-local materializations, invocation
  bindings and success/failure state;
- `BackgroundStorageRealization` interprets `BackgroundStoragePlan` indices for one
  selected range/version and produces explicit views, but does not decide demand,
  traverse nodes, invoke callbacks or publish; and
- the executor-level persisted-data store owns the only published persisted data;
  semantic completeness is transaction-owned rather than a fixed-record validity bit.

The workspace and prepared result are runtime components in their own right, not
nested executor implementation details. A generation realization may own and reuse a
workspace, but committed coverage remains a separate state object. The propagation
workspace's reusable arena is limited to:

- per-node/per-port forward-change accumulators;
- per-node/per-port reverse-requirement accumulators;
- forward/reverse region-set work buffers; and
- coverage callback frames for the generated F/R roots.

The surrounding transaction and storage realization, not the propagation workspace,
own:

- the prepared activity/request selection used by the selected Tock evaluation;
- selected exact persisted-data completion plans;
- background-produced `tock/ephemeral` result/intermediate sample/event values;
- temporary event data and event Region views; and
- temporary references to immutable stored snapshots and records.

The propagation workspace is reset once per prepared background operation. All
invalidation or demand roots assigned to that operation contribute into the same
accumulators before the corresponding traversal reaches a node. Transaction-local
values remain shareable across all consumers in the same batch. Once their last consumer is
complete, storage may be reused; future liveness packing is an implementation
optimization.

F/R success prepares a result; it does **not** commit semantic coverage. The prepared
coverage becomes authoritative only in the final transaction commit after all required
Tock/replay evaluation, materialization, candidate-completeness checks and stale-base
validation succeed. Abort or failure at any earlier or later phase discards the
prepared coverage and candidate data together. There is no public executor operation
that commits propagation alone; the complete transaction is the first runtime path
allowed to promote prepared coverage.
The successful prepared result snapshots node activity after reverse propagation.
Every reverse-demanded node also carries `evaluate`; forward-only activity still
propagates invalidation but does not schedule data evaluation. The coordinator passes
that immutable activity selection into the transaction-owned call frame before it
invokes the generated evaluate root.

`BackgroundExecutor` remains the lifetime/orchestration façade for background work: it
selects actor-owned inputs, constructs and executes the transaction, and publishes its
commit; persisted-record mutation, materialization execution and callback binding stay in the
components above.

```cpp
auto selected = select_pending_inputs();
BackgroundEvaluationTransaction transaction{
    generation, selected.roots, selected.producer_prefixes};
auto result = transaction.execute();
```

This is actor-internal pseudocode, not a public caller-driven operation.

Tock/ephemeral materialization and all Tock callbacks are scheduled off the audio
thread. Transaction-local addressable materializations needed by background Random
Access remain alive through their consuming evaluation; materialized Tick-time windows
survive until their callback readers release them. A Tick recording queue is
different: its logical backlog may grow with production duration and background lag,
so `AsyncCapacityManager` extends slab-backed storage while the audio-thread path
consumes only already-provisioned free blocks.

### Generated-root/runtime materialization boundary

`BackgroundEvaluationPlan` owns immutable execution facts, including runtime binding
slot maps and the placement of direct-view, conversion, projection, fan-in and other
materialization operations relative to background nodes. Runtime code must not recover
that schedule by walking `ConfiguredGraph`, compiler internals, or connection topology.
The plan therefore retains, directly or through an explicit evaluation-step schedule,
the operations that must run before a node invocation and any completion/finalization
work required after it.

The generated background evaluation root remains the owner of the statically ordered
node traversal. Immediately before and after each applicable node it invokes narrow
transaction-supplied prepare/finalize hooks from `BackgroundEvaluationCall`. Each hook
receives only an opaque transaction-local operation frame selected for that node. It
may execute the already-planned materialization operations and install or validate the
node's views; it is never given an executor pointer, the persisted-data store, a transaction
object, or another route to runtime internals. Generated code likewise receives only
the bindings and hooks it directly invokes.

Sample/event conversion, projection, direct delivery, fan-in and deterministic event
merge are small operations over explicit source/destination views. The transaction
coordinator selects versions and lifetimes; the storage realization resolves
`PortStoragePlan` indices into views; neither layer reconstructs compiler decisions.
Backing owners are made address-stable before the trivially-copyable callback frames
are formed, and remain alive until the generated root returns.

The concrete transaction-local assembly for that boundary is
`BackgroundEvaluationCallFrame`. It is non-copyable and non-movable, owns one opaque
operation frame per compiled node, and seals compact authored-Tock facades only after
the selected storage realization is sealed. Its logical binding coverage is aligned
with the compiler's dense binding slots rather than raw storage slots: this preserves
direct sample channel/read-latency mappings and permits one logical output write to fan
out to all selected destination stores. The same owner reserves the exact reflected
sample/event input/output arrays required by every replay slot. Once the transaction
installs the validated replay-region schedule, sealing allocates isolated power-of-two
sample/event storage and binds those arrays. The compiler-placed prepare/finalize hooks
populate replay inputs from the selected realization and flush replay outputs back;
call-frame assembly still does not choose coverage, activity or publication policy.

Replay invocation schedules are not raw, arbitrarily coalesced `Coverage` regions.
The immutable plan retains the applicable compiled root/primitive maximum block size
and replay binding slot; the transaction splits its dynamic required coverage into
legal invocation regions while preserving absolute indices and covered gaps. The
generated root invokes the imported `tick_block()` wrapper only with those validated
regions.

## 18. Background evaluation components and cycle validation

The **background evaluation DAG** contains tock-produced dependencies,
contextually replayable tick producers and the ordinary sequential input edges
traversed while replaying them. Published persisted outputs are stored boundaries:
the background evaluator reads their selected version and does not traverse their
live producers. Weakly connected evaluation components provide forward/reverse/
evaluation orders, stable port indices, coverage accumulators and callback
imports. Mechanically pointwise replayable Tick dependencies use compiler-generated
temporal propagation; non-pointwise Random Access dependencies use the authored
background propagation contract described above.

Whole-project semantic SCC analysis remains necessary for Tick-to-Sequential feedback and
for the conservative prohibition on a random-access **input** edge inside its own
semantic SCC. Additionally, the background replay dependency graph must be a DAG:
sequential edges traversed by replay can introduce a cycle even when no input is
labeled random-access. Such a path is not contextually replayable. An existing
published persisted boundary may terminate it when the requested finalized
coverage exists. No background evaluation may invent a new fixed-point/feedback
schedule; ordinary sequential SCC scheduling remains separate.

## 19. Node creation and coverage publication

Having `tock_coverage()` does not imply that a node evaluates merely because a
project or executable generation was created.

The primitive lifecycle event is **semantic concrete-node creation**: a node has
no retained compatible counterpart and is introduced into the background evaluation DAG.
Project startup is the case where every project node is newly created. Producing
new machine code for a stable retained node is not by itself semantic creation.

For a new node with Tock outputs (`tock/ephemeral` or `tock/persisted`):

1. ordinary node configuration/resources and optional background-owned `State` are
   initialized; `initialize()` may establish configuration/resource-dependent
   acceleration state, but not input-content-derived results;
2. current random-access-input coverage metadata is known, or an unresolved
   discovery requirement is registered before publishing output coverage;
3. `propagate_forward_coverage()` runs with a node-created/local-change cause and
   may have zero changed random-access-input regions; and
4. exact computed-output coverage is established before demand may target it.

For a newly created Tock output, old coverage is empty, so all new coverage is
an exact semantic output change and propagates downstream.

For a replayable Tick output whose availability mapping is mechanically pointwise,
GraphJit derives coverage from the exact available coverage of its transitive
dependencies. A replayable Tick node may also declare background propagation for a
non-pointwise Random Access dependency without acquiring a Tock output or
`tock_coverage()`. A tick/persisted output acquires
random-access coverage as its finalized values are published, whether or not
its producer is replayable.

A new explicit recorder output begins with empty coverage. A compatible recorder
that survives graph replacement retains its already-published RAM representation.
Queue publication itself does not publish output coverage. When a background
transaction selects new Tick/persisted recording queue prefixes, payload items
establish exact changed/added coverage and void items remove exact coverage at their
addressed ranges. The absence of an item leaves recorder coverage and values
unchanged. For Tick/persisted outputs, selected queue items supply finalized regions
to the private persisted-data candidate; only persisted-state publication extends the
Random-Access-visible retained snapshot.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Compatible stored state and
coverage are rebound unless semantic configuration, random-access input connection sets,
implementation semantics, sample rate, or another explicit forward cause requires
recomputation.

Under the 2026-11-16 destination, both Tock/computed and Tock/buffered outputs
may leave undemanded covered positions uncomputed. Each demanded published
window must be complete for its selected semantic/data version. The old
whole-coverage `tock/persisted` publication rule is superseded; see
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published coverage or persisted-output semantics enters
through a `BackgroundExecutor`-controlled boundary. Other code must not mutate
active persisted-data roots behind its versioning rules.

The architectural invalidation-root set is deliberately closed:

- node-local semantic state/resource mutation;
- independently selected finite prefixes from realtime-produced recorder and
  Tick/persisted queues;
- graph semantic configuration/topology/implementation change; and
- project sample-rate change.

Node-type-specific rules determine the exact initial changed regions for a
node-local mutation. Realtime-produced queue items identify their originating output
and global range, so the background executor coalesces selected items into exact
changed coverage per output. Graph/runtime rules
determine roots for the other classes. Once established, a random-access-input change at a
downstream node is merely propagation inside the same batch, not a new executor
entry point.

Conceptually, an ordinary UI/configuration mutation is:

```text
UI / project operation
        |
        v
executor-owned semantic mutation
        |
        v
update configuration/resource identity
        |
        v
create candidate semantic version
        |
        v
run exact forward coverage/change phase
        |
        v
prepare affected demanded output windows
```

The mutation may have zero changed random-access inputs and still change output coverage
or values. Several application mutations may be applied to one candidate and
added together before the batch's forward pass; intermediate semantic versions
need not be externally observable.

A random-access input **connection-set change** is another executor-controlled forward cause.
Addition, removal, replacement, or another semantic change to the set of
connections feeding one random-access input conservatively marks that whole logical
input as changed:

```text
changed_input = old_input_coverage | new_input_coverage
```

The node sees the new current input coverage plus that changed region. This may
overinvalidate unaffected fan-in portions but is finite, simple, and correct.

A recording bridge contributes queued changes differently from an ordinary semantic
edit. Before the background evaluation pass starts, `BackgroundExecutor`
independently pins a finite prefix from each relevant Tick/persisted recording
producer queue. Each prefix is immutable for the selected workload. Blocks appended
while propagation or Tock is running are not added to the current workload even when
they target earlier global positions; they wait for a later pass.

The selected recording payloads start ordinary forward invalidation. The bridge's
background operation applies payload overwrites and explicit void erasures to the
candidate RAM recording as part of the same reverse/Tock transaction as downstream
work. Only the final transaction commit publishes the coherent successor persisted
state/data version.

Queue publication itself is a complete background-work trigger. The worker pins the
available finite prefixes and derives the originating output, covered timeline ranges,
and changed ranges from the generation-local route plus each record header. Processing
those prefixes is not gated by an explicit evaluation call, an external coverage
request, or a previously successful background operation.

`BackgroundExecutor` owns the transaction environment used for that work. Persisted
data have no root-block-derived page width. Semantic version is selected from the actor-owned
semantic state. Additional node mutations and external/advance demand are independently
accumulated actor inputs and may be coalesced with a queue-driven batch; they are not a
monolithic caller-supplied `BackgroundEvaluationRequest`.

External reads have a correspondingly closed entry-point set: application/UI
random-access fetches and advance background materialization for sequential playback.
The live `CompiledGraph::tick_block()` reads published/materialized data or supplies
its consuming sequential input's neutral value; it never runs tock.
Stale or absent `tock/persisted` ranges are internal materialization roots, not a
third external caller. The external protocol may combine updates and
fetches in one request; `BackgroundExecutor` normalizes that shape into mutation roots,
demand roots, and one or more legal background evaluation transactions.

## 21. Semantic versions, data versions, and stale-work rejection

Background computation may overlap Tick execution and may be superseded by newer
edits or by recording items that arrive after a running batch pinned its inputs. The
published persisted output state uses two version coordinates:

- **semantic version** identifies graph/configuration/resource/sample-rate semantics;
- **data version** identifies one immutable published persisted-data root produced by
  a completed background evaluation transaction under those semantics.

A queued Tick recording item does **not** advance the data version. Items accumulate
as pending transaction input. A data version advances only when the background worker
has processed fixed producer-queue prefixes, propagated the transaction's invalidation roots,
completed any required Tock/replay work, filled the affected persisted candidate
records, and atomically committed the successor snapshot. A semantic edit may likewise
create a new semantic version whose retained data are built before publication.

Background evaluation transactions therefore operate against a coherent base/target pair:

```text
(semantic_version, data_version)
```

The reverse/evaluation phases of a background evaluation transaction use one
immutable base data version for existing random-access reads and produces one candidate successor. It never switches its base to
a newer publication in the middle of evaluation. An already-running transaction
may finish against an older immutable base, but stale-work/version validation must
prevent it from being committed as current when its semantic assumptions are no
longer valid.

For `tock/persisted`, a candidate is publishable only when every required computed
output is complete over its target coverage for the transaction. Unchanged records
and range-tree subtrees may be structurally shared from the immutable base. For
`tick/persisted`, the candidate structurally shares the prior snapshot and incorporates
all authoritative recorded callback blocks in the selected producer-queue prefixes;
the live Tick producer is not replayed merely to fill those ranges.

For ephemeral Tock/replay results, no persisted-output validity exists; the selected
version chooses the configuration, coverage, sample rate, and immutable persisted
upstream data against which the exact request is evaluated.

A queue-consuming pass owns the independently selected `(first,last)` prefix from
each relevant producer queue. The candidate may read exactly those selected blocks
plus already-published retained state. Blocks linked after a selected terminal block
are invisible to that pass and cannot change its coverage, invalidation roots,
reverse requirements, or Tock/replay inputs.

If the pass commits, publication promotes the prepared semantic/data state and the
selected queue prefixes may then be released according to their domain semantics. If
the pass is cancelled, fails during evaluation/materialization/publication, or is
rejected as stale, the committed semantic/data baseline does not advance and required
pending queue input remains available for retry rather than being silently consumed.

## 22. Direct connection compatibility and explicit recording

Connection validation independently checks output shape, realization, input
access, consumer content-access capability, type, timing and execution
availability. A Coverage output **never** connects directly to a Region
input. A Region/computed output cannot satisfy a complete-buffer Region
input; the author can insert an explicit materialization node that bounds
the requested range and produces Region/buffered. A Region/buffered output
can supply the full contiguous pointer once that Region has actually been
prepared, whereas both realizations can supply planned windowed reads.

A Tick/Sequential/buffered output is an ordinary recording producer.
`write_void()` erases positions, `untouched` preserves previously recorded
positions, and writes overwrite addressed positions; queue staging precedes
immutable background publication. Historical recording reads are
Coverage-shaped and cannot directly feed Region inputs. An unreproducible
Tick/Sequential/computed stream requires an explicitly authored buffered
recording boundary for historical Random Access demand. An eligible Tick
computation may instead be replayed where every upstream dependency is
reproducible and the replay DAG is acyclic. Feedback `.detach()` is a
non-replayable history boundary.

Tock-produced Region or Coverage data feed a Tick consumer's Sequential input
through prepared bounded windows. The source remains a Region or Coverage output. Realtime Random Access likewise uses only prepared pinned windows
or complete buffers; no callback can cause background work or allocation
synchronously. Output realization does not select cache retention. See
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md) for the normative compatibility matrix.

### Storage requirements are inferred over overlapping port subsets

Connection compatibility is checked per contribution, but **storage is not
chosen per connection**. A source channel can fan out to several differently
configured inputs, and only a subset of those channels may participate in another
connection. The inverse is also true for target inputs with overlapping fan-in.
GraphJit must therefore partition source and target port elements into maximal
**port atoms** with identical incidence before joining storage requirements.

For samples, begin with individual source/target channels. For events, a whole event
source/target may be the initial atom unless routing semantics introduce a finer
partition. Two elements stay in one atom only when every relevant fact matches:
connected peer subsets, conversion/projection, timing mapping, destination access,
callback-use context, and composition semantics. This partition is exact and may
split one authored output several ways.

Example:

```text
output O = {a,b,c,d}

Sequential I1     <- {a,b}
Random Access I2  <- {b,c}
Sequential I3     <- {d}
```

produces distinct incidence signatures for `a`, `b`, `c`, and `d`; only atoms whose
full requirements later prove equivalent may be coalesced. Conversely, a
pair of channels selected together by every connection with identical timing and
conversion facts may remain one atom.

For each source atom `S`, join the intrinsic output requirements with every outgoing
use:

```text
requirements(S) =
    intrinsic production/retention requirements
    UNION
    all consumer access/lifetime requirements
```

For each target atom `T`, independently derive the representation needed to compose
all incoming source atoms. Direct channel placement can remain a collection of
views; arithmetic mixing or conversion materializes only the affected target atom.
Event fan-in additionally preserves deterministic equal-time source ordering.

The minimum capability implications are:

```text
if S is persisted:
    require canonical persisted data
    require candidate/published data-version state and reader pins

if S is Tick-produced and any same-Tick Sequential use exists:
    require current Tick representation

if S is Tock/ephemeral and any Tick-time Sequential use exists:
    require materialized sequential window

if an ephemeral Tock/replay result has any Tick-time Random Access use:
    require materialized immutable addressable window

if an ephemeral Tock/replay result has background-only Random Access use:
    require transaction-local addressable materialization

if S is unreproducible Tick/ephemeral and any Random Access demand reaches it:
    reject implicit storage; require authored persistence/recording
```

These requirements form a capability join, not a single strongest storage enum. A
materialized addressable window can also provide sequential slices for the same range,
so those uses may share storage. In contrast, `Tick/persisted -> Sequential` plus
`Tick/persisted -> Random Access` requires **both** a current Tick representation and
a published persisted-data representation: their visibility semantics are different.

`RegionInputConfig` or `CoverageInputConfig` by itself does not reveal whether node
code reads that input from Tick, Tock, or both. GraphJit should use statically known callback access
when available; until that is represented precisely, the safe planner rule is to
join every legal callback context for that input.

Derived converted/composed representations may be shared only when their source-atom
set, conversion/composition, temporal mapping, selected semantic/data version,
requested range, and lifetime/access contract match. This is what permits useful
fanout sharing without allowing one overlapping subset to promote or corrupt another.

### Preliminary Random Access visibility

The first implementation intentionally uses the simpler **published-snapshot-only**
Random Access rule. A Tick callback selects/pins its immutable persisted-data view at
the callback boundary. For a Tick/persisted producer, a block produced or captured
earlier in that same callback is therefore still invisible to Random Access even if
its eventual retained position is already known. Private candidate records and sealed
capture records do not extend the selected view's coverage.

A later data-version publication makes those positions available to a subsequent
callback/read context. Publication occurring concurrently with a callback does not
mutate that callback's pinned view. Consequently, under this preliminary rule a
Tick/persisted-to-Random-Access connection adds **no same-Tick scheduling dependency**:
running the producer earlier in the callback would not change the snapshot the
consumer is allowed to observe.

This is deliberately more conservative than a possible future recent-data overlay.
Such an optimization could allow a downstream Tick Random Access consumer to read a
newly finalized Tick/persisted capture during the **same Tick**, but it would require:

- a same-Tick scheduling dependency from the producer to that consumer;
- an immutable recent-capture lookup layer in front of published data;
- a branch/split when a requested range crosses from published data into recent
  captures (and an ordered two-source merge for events);
- one semantic/version lineage so newly published data supersede, rather than
  duplicate, the recent overlay; and
- callback-boundary-safe block reclamation.

The preliminary implementation does not pay that complexity.

Tick-visible ephemeral advance materialization is not a published persisted-data
version. The runtime object that pins those immutable views for one callback should be
named `TickMaterializationSnapshot` (or an equivalently explicit local name), not a
"published materialization." It has one owner/selection path, is selected at the
root boundary, and remains alive through all callback readers, but it does not acquire
persisted-output retention or data-version semantics.

### Sequential playback and persisted-data availability

A sequential input consumes its selected pinned published data **as-is**, even if
that data are stale for a pending candidate. A genuinely missing position
produces that **input's own `neutral_value`** (including when members of a tile
have different neutral values). Stale does not mean missing: old published data
remain readable until a replacement is atomically published. The audio thread
never waits for a background evaluation transaction, invokes `tock_coverage()`, or allocates
persisted data on demand. A coherent pinned snapshot prevents concurrent publication from
mutating memory under an audio callback. The neutral fallback does not authorize
out-of-coverage arbitrary random-access reads by node code; those remain invalid.

While playback is paused, background-produced Sequential continuous inputs are treated
as missing and therefore read their own `neutral_value`; background-produced
Sequential event inputs are empty. Region and Coverage inputs expose empty extents.
The Tick graph itself continues executing, including live and ephemeral signal paths.
Resume selects the applicable immutable background snapshot at a callback boundary and
may jump the shared node-visible `FractionalGlobalIndex`; it does not reset live
Sequential history or introduce a second node time coordinate.

### Provisioned queue items and Tick/persisted recording boundaries

Realtime-produced data whose lifetime must escape ordinary current-block execution
uses producer-specific producer reserves and background pending queues. An explicit
recorder is simply an authored node whose Tick/persisted output stages finalized data
for canonical persisted-data publication; it may consume otherwise unreproducible Sequential
input. Production occurs at the producer/
finalization point, not through a graph scan after the whole pass.

Every Tick/persisted output has invocation-local disposition reset before its node
invocation. Ephemeral Tick outputs and all Tock outputs do not observe or update this
state, so their ordinary write paths pay no disposition cost:

```text
untouched -> publish nothing; preserve recorded RAM at the range
written   -> publish payload; overwrite recorded RAM at the range
voided    -> publish explicit erase at the range
```

Ordinary Tick/persisted output-authoring operations mark the output `written`;
`write_void()` marks it `voided`. Writing and voiding the same logical block are mutually exclusive. This
is observed state of ordinary port authoring, not recorder policy and not a second
callback API. The statically specialized Tick/persisted facade performs this
observation while forwarding the ordinary authoring operation; the low-level generic
sample/event port objects carry no recording pointer or state. When GraphJit supplies
the default skipped-block silence and empty-event outputs, it uses those same ordinary
facades. Ephemeral Tick outputs and all Tock outputs instantiate facades without
observation and pay no dynamic recording check.

For `tick_block()`, the disposition applies to the positions the addressed callback
block finalizes: for a block beginning at `B` with `N` positions and an output latency
`L`, `[B - L, B + N - L)`. Those are the positions no later block can change: the block
may still revise sample values or add events in its latency window `[B - L, B)`, and
leaves its own newest `L` positions open to the next block. `written` publishes one
authoritative overwrite of that range and `voided` publishes one authoritative erase of
it. Scalar `tick()` has the same rule with a one-sample block. History does not extend a
recording item. Recording start and stop precision therefore follows Tick callback
boundaries, shifted by the output's latency. (Newly settled 2026-11-10, human
co-author; the superseded text, under which the item covered the callback block itself,
is preserved in
[historical/persisted_recording_callback_block_2026-11-10.md](historical/persisted_recording_callback_block_2026-11-10.md).)

An explicitly authored zero-event Tick/persisted block is distinct from both an
untouched output and a recording void. Calling the ordinary block-authoring operation
with an empty event block marks the output `written` and publishes an empty payload,
so older events in that covered window are replaced by emptiness. Merely producing no
events and never invoking an output-authoring operation leaves the output untouched.

Where layout permits, payload data is written directly into capacity-manager-
provisioned blocks. The producer may link several blocks privately, fully
initialize their payload/header metadata, and publish the completed chain at a
realtime pass boundary. Publication transfers the chain to the appropriate producer
queue in `BackgroundExecutor`; no global capture insertion sequence is required
by the transport itself.

Domain-specific ordering remains explicit where semantics require it. For example,
multiple overlapping recording writes produced by one ordered producer stream are
applied in that stream's queue order. Seeking may therefore publish later changes for
numerically earlier timeline positions without turning timeline position into queue
order.

### Independently pinned queue prefixes per background pass

At the beginning of one background pass, the worker independently pins the currently
visible finite prefix of every relevant producer queue. For each queue it remembers a
terminal block before execution begins. Later publication may link more blocks after
that terminal block, but they belong to a later workload.

There is intentionally no globally atomic snapshot across these queues. If queue A is
pinned before a concurrent publication and queue B is pinned after one, the resulting
mixed wall-clock cut is valid. If a future semantic operation requires several inputs
to become visible atomically, that operation must package/version its dependency
explicitly or receive a dedicated design; the generic queue primitive does not grow a
global synchronization mechanism.

Selected recording payloads are applied as overwrites and selected void items as
erasures in the producer stream's order. Their exact changed/added/removed ranges
become ordinary invalidation roots. From that point forward there is no recorder-
specific downstream scheduler: normal forward propagation determines downstream
changed coverage, reverse planning determines required input coverage, and ordinary
background evaluation materializes the recorder output and affected downstream data.

Queue blocks are transaction-input storage, not a published output view. Coverage/
Tock callbacks never observe blocks outside the selected prefixes, and queue insertion
never changes a workload already being executed.

### Generation cutover and queued data

Realtime/background hot reload does not reinterpret queued data under a newer graph.
Producer endpoints/queues are generation-specific by default. At the authoritative
realtime pass-boundary cutover, realtime first publishes all final old-generation
chains, then publishes the already-prepared generation cutover, then begins using only
new-generation producer endpoints. The old queues are therefore closed and have finite
tails.

The background worker may continue executing previously selected old-generation work
after realtime has switched. It drains/finalizes the closed old-generation inputs and
applies the prepared persisted-state migration before interpreting new-generation
items. This does not require a global atomic snapshot across queues.

Stable logical output identity controls retained-state survival. If a recording
producer disappears but its destination survives, the final old-generation recording
is preserved and no later writes arrive. If the destination disappears, remaining
old-generation writes are still valid old-generation work and the retained destination
is retired during the background generation transition. Installation order therefore
does not determine whether a queued write is retained or discarded.

Persisted-state versions published to realtime identify the graph generation with
which they are compatible. A late final old-generation version is background transition
input, not a version that may be installed into the newer realtime generation.

See
[realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md)
for the normative cutover protocol.

## 23. Demand-complete buffered outputs and backing storage

A buffered output may publish its exact logical coverage and Region list
without materializing every covered position. The executor tracks which
content windows have been prepared and validated for each immutable data
version. A requested window is published only when **that window** is
complete; an uncomputed covered position remains a dependency, not absence.
Previously prepared unchanged windows can be reused in newer coherent
versions. Source data needed to reproduce a version (notably committed
recordings) remain authoritative; caches of recomputable content and
prepared buffers can be evicted, remapped or regenerated.

A full-buffer Region consumer is an explicit exception: every sample of the
selected logical Region must be prepared, pinned and contiguous before the
consumer executes. If that requirement exceeds the available resources, fail
before attempting an uncontrolled allocation. For ordinary Sequential and
windowed random-access consumers, neither buffered nor computed outputs
require whole-Region or whole-coverage production. Linux mmap, sparse pages,
file backing and copy-on-write roots are optional storage realizations;
none changes the demand semantics.

A Tick/Sequential/buffered source differs from a Tock/buffered source only
in the population mechanism and producer queue timing. Both use one shared,
background-managed versioned buffer resource and backend-selection policy:
where appropriate, sparse Linux virtual mappings can place newly computed
pages at their final offsets without copying existing unchanged pages. The
executor limits both the per-view mapping span and aggregate reserved virtual
address space. It also budgets process-wide VMA fragmentation, physical
backing and realtime-resident prepared pages independently. Physical backing
runs should be mapped together when possible; a logical page is not
necessarily one VMA. Obsolete backing must be reclaimed/reused only after
all version and reader pins release it, without discarding authoritative
recorded contents. Large logical Regions can still be served through bounded
windows; complete-buffer consumers cannot be given segmented pointers as if
they were contiguous. Published versions provide the same coherent read
boundary. See [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md), §8.

## 24. Realtime-produced persistence/recording queues and reclamation

Realtime-produced work whose lifetime escapes the current pass uses the generic
`GraphExecutor` reserve/queue infrastructure. Explicit recording and Tick/persisted
staging are important users, but the mechanism is not recording-specific.

The ownership path is:

```text
AsyncCapacityManager
        |
        v
producer-specific ProducerReserve
        |
        | RealtimeExecutor takes already allocated blocks
        v
producer-private chain written during realtime execution
        |
        | one cheap publication at a pass boundary
        v
BackgroundExecutor producer-specific PendingQueue
        |
        | worker independently pins one finite prefix
        v
background evaluation / persisted-state publication
        |
        | committed blocks enter one released-block stream
        v
AsyncCapacityManager
```

### 24.1 Direct realtime production

Realtime execution never allocates request-sized storage. A producer consumes only
blocks already assigned to its reserve.

Where layout permits, sample/event output writes directly into those blocks. For
recording this is the preferred representation: destination/range/disposition metadata
and the final sample/event payload can live in the queue block itself, avoiding a
second capture-record/payload layer and a second memory copy. Where direct production
is not legal, only the bounded copy required when the final authored region becomes
available is performed.

The producer can build one or more blocks privately:

```text
[A] -> [B] -> [C] -> null
```

Payload, used counts and private links are ordinary writes while producer-owned. At the
pass boundary, `(first,last)` is published; background attaches the fully initialized
chain to that producer's pending queue with one pointer publication.

Recording semantics remain fixed:

```text
ordinary write -> publish overwrite data
untouched      -> publish nothing; preserve prior RAM recording
write_void()   -> publish explicit authoritative erase
```

A Tick/persisted zero-event window is ordinary authoritative empty event data, not a
recording void. Failure to obtain mandatory already-provisioned capacity is an
observable retention/recording failure; later replenishment cannot reconstruct the
missed write.

### 24.2 Capacity policy

`AsyncCapacityManager` runs off realtime and independently of background progress.
The producer supplies:

```text
C = maximum burst that must fit without provisioning
L = low ready-capacity watermark
H = refill target
```

For graph-generated realtime work, `C` covers the maximum block demand of one
worst-case pass. `L` covers tolerated manager detection/scheduling delay plus margin;
`H` supplies headroom/hysteresis. `C` is used to validate/derive policy. The allocator
chooses its own power-of-two slab/allocation granularity and refills below `L` toward
`H`.

The queue users do not need a shared atomic logical size. The capacity manager may keep
its own accounting. Slow background execution may retain old blocks while the manager
allocates additional reserve; there is no fixed-duration ring whose fullness permits
ordinary loss.

### 24.3 Background selection

Every producer has its own pending SPSC queue. Before one background operation starts,
the worker independently discovers and remembers a finite terminal block for every
queue it chooses to consume:

```text
selection:

A -> B -> C -> null
^         ^
first     selected last

later publication:

A -> B -> C -> D -> E -> null
          ^
          current work still ends here
```

The selected workload uses pre-sized descriptors owned by the prepared execution
generation; selecting work does not require dynamically growing a map/vector merely to
remember queue prefixes.

There is intentionally no atomic snapshot relationship across queues. A publication
racing selection may enter the current operation for one producer and the next
operation for another. If a future feature requires cross-queue atomic visibility, it
must be designed explicitly rather than adding hidden global synchronization.

### 24.4 Commit, publication and reclamation

Queue blocks are background inputs, not a second retained Random Access representation.
A successful transaction commits the semantic/data result corresponding to its selected
work. Only after successful domain commit may the selected prefixes be released. A
failed/cancelled/stale transaction does not silently consume input required for retry.

Released blocks from all producer queues may enter one SPSC released-block stream from
the single background worker to `AsyncCapacityManager`. Each block carries enough
owner/type information for reclamation/reassignment. Background never directly splices
completed blocks into a realtime reserve.

When background produces a coherent immutable persisted-state/data version, it
publishes it through a latest-version mailbox to realtime. Intermediate pending
versions may be superseded before realtime reaches a pass boundary; superseded objects
are retired off realtime. Realtime activates only the newest compatible pending pointer
at a legal boundary.

If persisted-data storage adopts queue payload storage, ownership must still be
explicit: adopted storage belongs to that immutable data version until it retires,
and only non-realtime reclamation can return it to reusable capacity.

### 24.5 Generation cutover

Producer reserves and pending queues are generation-specific. `GraphExecutor`
constructs one complete `ExecutionGeneration` off-thread, including both actor
realizations, routes/endpoints, pre-sized work descriptors, migration information and
reserve requirements.

At cutover, realtime publishes final old-generation chains, publishes the prepared
successor `ExecutionGeneration*`, then swaps active generation. Actual cutovers remain
ordered while background lags; the generation object itself may carry intrusive
successor linkage so no cutover allocation is needed at the realtime boundary.

Old-generation producer endpoints are then closed. Their queues have finite tails and
remain interpreted under the old graph while background finishes/drains them. Stable
logical identity determines migration: a disappeared producer stops future writes but
does not erase a surviving recording destination; a disappeared destination accepts
remaining old-generation work and is retired during migration.

## 25. Random-access event/sample reads from node callbacks

Random-access sample reads address global covered sample positions. Tick/persisted
and Tock/persisted outputs use the same published persisted-data read path. At the
callback boundary, realtime adopts at most the newest generation-compatible mailbox
root, which already pins one exact persisted-data/Tick-materialization pair. Current
Tick buffers, pending candidates, and newly queued realtime blocks are not alternate
lookup sources.

Ephemeral Tock/replay results use immutable addressable materializations instead of
persisted data. Background-only consumers may use transaction-local materialization.
A Random Access input used during Tick execution requires the needed ephemeral result
to have been materialized into an immutable addressable window before the callback.

Random-access event reads may span several stored chunks and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Random-access accessors expose the selected input view for the invocation:
Tock receives complete explicitly provided Regions, and replay/Tick receives
its selected prepared view. In the intended Tock API, `coverage()` is the union
of the provided Regions, not a separate reverse-demand mask. Debug validation catches
requests outside coverage. A `tock_coverage()` accessor reads Random Access inputs from the selected immutable
transaction view, which may include canonical persisted data regardless of Tick/Tock
provenance, background-replayed Tick values, transaction-local Tock/replay
materializations, and an explicit recorder's published output. It never reads mutable
current Tick storage or queue blocks published after the selected workload boundary.

## 26. External random-access result lifetime

Application/UI callers should not receive unscoped raw pointers into executor-owned
immutable storage whose snapshot may later be reclaimed.

Two valid host-side result models are:

- return owned/copy results; or
- return an explicitly scoped/pinned immutable view that keeps the referenced
  stored snapshot/representation alive for the view lifetime.

The simplest initial application boundary should prefer owned results. Generated
internal tock/reverse execution may use borrowed transaction/stored views whose
lifetime is controlled by `BackgroundExecutor`.

### External version selection and incomplete candidates

Every externally returned random-access result is associated with the
`(semantic_version, data_version)` pair from which it was read.

For `tock/persisted`, a newer candidate is not externally presented as a partially
materialized stored output. Until the entire required stored candidate is complete,
the newer version is pending/not-ready and callers may continue using an older
completed published result. Missing candidate ranges must never be represented as
neutral samples, zero events, or artificial missing coverage.

For `tock/ephemeral`, an external request may evaluate exact requested coverage
against a selected immutable version pair and return that pair with the result. The
host API may satisfy the request synchronously or expose explicit pending/future
behavior; the exact ABI is implementation work.

For Tick/persisted output, sealed captures newer than the selected published data
version are likewise pending, not an alternate external read source. The old
published snapshot remains coherent until a successor incorporating the fixed capture
prefix commits. This is the deliberate preliminary "out-of-date read" behavior.

A caller must not combine independently returned data from different semantic or
data versions as if it were one coherent random-access result.

## 27. Outward change notification

After a candidate data version becomes published—including a transaction that
consumed recorder or Tick/persisted capture records—the executor may emit a coalesced
output change notification for subscribed external consumers.

Conceptually it contains:

```text
output identity
semantic version/revision
published data version
new IndexCoverage (or a coverage-changed indication)
changed IndexCoverage (sound may-change superset)
```

Notification is emitted from the executor publication/change boundary, not
recursively from individual callbacks. Presentation/UI code can react by issuing
ordinary random-access requests. The notification does not itself force
`tock/ephemeral` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and semantic invalidation are separate events. Producing a new
`CompiledGraph` generation does not by itself make any stable persisted output
stale or revoke its retention guarantee.

`BackgroundExecutor` reconciles old/new background generations using stable concrete-node/output
identity. Compatible persisted outputs rebind to the existing stable store rather
than copying data merely because machine code changed.

Important cases:

- tock/persisted with unchanged semantics preserves coverage and data;
- tock/persisted with changed semantics runs exact invalidation and rebuilds
  stale candidate ranges before publication;
- tick/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no persisted data to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic port identity rather than builder
handles, lowered node indices, or ORC-generation-local indices.

The executable local-continuity key is deliberately not a transitive graph hash. A
compiled background node carries:

- stable concrete-node identity and registered primitive identity;
- the exact provider-definition version;
- a shared typed-configuration ownership token retained only while the complete
  recursive configured-definition closure remains version-compatible; and
- its compiled background execution mode and semantic input/output port shape.

The handoff compares these facts with the exact active predecessor. A mismatch emits a
local-node invalidation root. Stable inputs are compared separately through normalized
incoming descriptions containing stable source-output identity, delivery mechanism,
type/conversion/projection facts, and target-channel mapping. A changed incoming set
emits an input root whose changed range is the union of the final predecessor input
coverage and successor input coverage. Existing forward propagation, rather than a
stored dependency fingerprint, carries both root kinds to all downstream outputs.

Definition-snapshot publication may preserve cached configured instances whose entire
recursive definition closure still matches. Replacing a definition invalidates all
cached parents which depend on it, including every instance of a broadly reused node
kind; unrelated definition publication does not invalidate them. A sample-rate change
is an explicit whole-plan local-semantic change because coverage callbacks observe the
rate. Initial graph loading has no predecessor and constructs fresh state.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output Tick root. It does
not gain synthetic output ports for background evaluation or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal coverage-planned ports to
generated background-evaluation component executors, persisted bindings,
advance-materialization plans,
and explicit recording-bridge capture/output identities. Static background-evaluation topology is
specialized during lowering rather than rediscovered for every request or block.

The generated background evaluation root also owns the static interleaving of node
invocations with transaction-supplied prepare/finalize hooks. The immutable plan maps
each hook and callback port to a compact runtime binding slot. Generated code calls
only those narrow hooks and consumes only those slots; it never receives an executor,
persisted-data-store or transaction pointer.

The generated Tick root remains a zero-project-port root but gains an explicit
invocation frame for dynamic background-derived bindings. GraphJit retains the
compile-time mapping from node ports to compact frame slots, and `RealtimeExecutor`
selects one immutable persisted-data snapshot plus any `TickMaterializationSnapshot`
at the root boundary. The frame contains views/bindings only, not storage-discovery
objects or ownership backdoors.

### Reuse whole-graph analysis products during lowering

Whole-project lowering reuses one SCC decomposition of the complete semantic
dependency graph to reject random-access input edges within a semantic SCC.
Separately, expand the background evaluation DAG through sequential input edges
of replayable tick nodes. Reject any unresolved replay dependency cycle; a valid
published persisted output is a terminal stored boundary for its covered requests.
Checking only labeled random-access edges is **not** sufficient for this second
check. Reuse the same stable indices and graph-analysis data; do not perform a
new reachability search per output.

SCC IDs, member ranges, condensation-DAG/topological order, dense node indices,
and reusable adjacency storage should be carried forward where useful. Background
component planning, same-Tick scheduling, liveness/storage planning, access/retention
lowering, and later batching/fusion/vectorization should consume those facts
instead of rebuilding equivalent graph views.

This is an efficiency rule, not a semantic requirement to collapse distinct graph
relations into one. Explicit-DAG validation, detach legality, same-Tick
dependencies, and complete semantic dependency relations answer different
questions. Detach validation may still require pre-feedback reachability. Share
indices/storage/traversal scratch where the edge relation actually matches.

The initial compiler should recompute SCCs from scratch for each graph compilation
rather than implement dynamic SCC maintenance. A complete Tarjan/Kosaraju-style
pass is linear, simple, deterministic, and expected to be negligible beside LLVM
lowering/optimization until profiling proves otherwise.

Execution ownership is internal to the `GraphExecutor` app module and split between
two actors. `RealtimeExecutor` owns canonical realtime `NodeStorage`, active/pending
realtime generations, pass-boundary activation and realtime queue producer endpoints.

`BackgroundExecutor` owns:

- stable canonical persisted-data stores/immutable roots for **all persisted
  outputs** (`tick/persisted` and `tock/persisted`), plus per-generation bindings;
- semantic versions plus immutable data versions and candidate/published snapshots;
- producer-specific pending queues and their consumer-side prefix ownership plus pre-sized selection descriptors;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/Tock transactions;
- completion scheduling for candidate Tock/persisted outputs and incorporation of
  selected Tick/persisted queue prefixes; and
- external Random Access request/change-notification lifetime.

`AsyncCapacityManager`, also owned by `GraphExecutor` as runtime infrastructure, owns
non-realtime provisioning/recycling of queue blocks. No mutable state is jointly owned
between the realtime and background actors.

## 30. Deliberately open implementation/tuning choices

The executor may choose physical backing, window sizes, cache eviction,
read-ahead, range-tree/directory representation, buffer pooling, queue block
sizes, cancellation granularity, mmap/file backing, and SIMD/fusion cost
models. These choices must not affect output values, exact logical Region
structure, authoritative recording information, dependency validity,
versioned snapshot coherence, or realtime deadlines.

The following remain semantic requirements, not optional optimizations:
static constexpr concrete schemas; Sequential outputs for Tick and
Region/Coverage outputs for Tock; computed/buffered output realization; a separate
windowed/complete-buffer Random Access input capability; Coverage-to-Region
prohibition; explicit bounded materialization where required; exact
`IndexCoverage` and sound forward/reverse propagation; no incidental cache
influence on selected Tock inputs; partial demand-complete buffered
publication; separately preserved recorded snapshot data; contextual replay
with no unresolved delayed feedback SCCs; background-only Tock and
propagation; immutable prepared input windows in Tick; audio-safe
publication/reader pins and reclamation; and exact committed producer-queue
prefix semantics. The earlier strict retention/whole-Region rules are
preserved in historical documentation and superseded by
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

## 31. Static validation

Target node/package validation rejects non-static or non-constexpr concrete port
schemas at **every** construction path (including internal builder paths), malformed
authored `State` in either execution domain, invalid event capacity
and invalid replayability trait declarations. Tick permits only Sequential
outputs; Tock permits only Region or Coverage outputs. A primitive selects
exactly one primary Tick or Tock callback family. The planned batch
API additionally rejects ambiguous authored shapes: a primitive authors exactly one
primary form across `tick()`/`tick_block()`/`tick_block_batch()` or
`tock_coverage()`/`tock_coverage_batch()`, with the same disjoint output-shape
rule for scalar and batch callbacks; skip plus each authored
propagation direction choose one supported synchronous scalar, coroutine scalar,
or synchronous batch form as specified in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

The following is a source-status checkpoint and retains legacy type names:
the checked-in replayability validation accepts `tick()`, `tick_block()`, or
`tick_block_batch()` through the normalized `do_tick_block()` operation. It rejects
`State`, `TockState`, Random Access inputs, nonzero port history or latency, and
nonzero internal latency. This is an implementation checkpoint, not the target
semantic rule. The target replayability trait removes the prohibition on Random
Access inputs. It retains the deterministic, side-effect-free, state, history, and
latency restrictions. Front 2 removed the callback-shape special case. The replay
contract still requires pure deterministic evaluation under a fixed version.

Whole-project GraphJit validation resolves per-channel connections and semantic
SCCs, derives contextual replayability and exact available coverage, rejects
unreproducible Tick/Sequential/computed historical Random Access demand
unless an explicit recording node intervenes, and rejects unresolved
background evaluation cycles. It also rejects Coverage-to-Region and
computed-to-complete-buffer input connections; the latter require an
explicit bounded materialization node. A Tock -> Random Access consumer
requires version-consistent prepared windows or complete buffers as declared;
cache lifetime belongs to the executor. Explicit recorder bridges must have their valid authored
input/output shape; any realtime-produced queue plan must have realtime-safe pre-provisioned
capacity and pass-boundary-safe ownership/reclamation. Unsupported event-
replay contracts must be rejected, not silently treated as sample replay. Errors
identify the specific source channel, input, dependency
path, or offending node where practical.

Plan validation additionally requires every runtime background binding slot to have
one compatible producer, every planned materialization input/output index to be in
range, every before/after-node operation placement to respect background dependency
order, every persisted binding to have stable or explicit generation-local identity,
and every replay invocation to respect the compiled maximum block size. Missing facts
are compiler-plan errors; the executor must not repair them by rediscovering topology.

Transaction/runtime tests cover failure during F, R, authored Tock, replay,
materialization, candidate validation and publication; every case must leave committed
coverage, the published snapshot and selected queue-prefix ownership unchanged. They also cover
stale-base rejection, old-or-new whole-snapshot reader visibility, complete exact-range
validation, stable and anonymous output identities, authoritative empty event chunks,
deterministic equal-time event ordering, missing Sequential-input neutral values, and
the prohibition on audio-thread Tock/allocation/blocking/final-owner reclamation.

## 32. Prior implementation landing order (source checkpoint)

The steps below document the pre-2026-11-16 implementation sequence and
checked-in source checkpoints; **they are not the normative remaining-work
order** after the computed/buffered redesign. See
[Automation And Background Planning Migration](./automation_and_background_migration_plan.md)
for the superseding feature dependencies, and
[Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md)
for the intended architecture. The earlier source checkpoints are retained
here as implementation history.
The legacy executor deletion, previous schema/intrinsic trait changes, and the
connection/background-dependency planner rewrite were separate landed stages
before GraphJit background execution. Through step 3, GraphJit retains orthogonal
production/retention/access/delivery facts, contextual replay proofs, and a
background evaluation DAG; it still does not execute Tock/replay work or imply
recording merely because that planning metadata exists.

1. **Landed: delete legacy execution and dynamic concrete-port declarations.**
   `ModuleLoader` now publishes the configured graph and derives source
   introspection directly from it. The `GraphLowerer`/`GraphCompiler`/
   `RuntimeGraphRoot` path, generated routing nodes, type-erased executor/facade
   fallbacks, and old runtime-root ABI are deleted. Public and internal concrete-
   node construction require static constexpr port schemas; graph topology and
   static-schema per-instance connection metadata remain dynamic.
2. **Landed: final port schema and intrinsic replay trait.** Carry independent
   input access, output production and output retention through reflection, static
   port lookup, configured graphs, the version-bumped archive and compiler records.
   Delete inferred cross-axis conversions and obsolete API names. Validate the
   authored, deterministic, side-effect-free `tick()`-only replay contract,
   including zero configured internal latency, and reflect it into retained
   compiler metadata. Preserve authored F/R/T callbacks only for Tock outputs.
3. **Landed: connection and background-dependency planning.** Replace the
   equality-based two-domain connection gate with per-channel production,
   retention, destination-access and delivery facts. Derive contextual replay by
   backward traversal through eligible sequential dependencies to persisted
   boundaries, reject live sources and unresolved cycles, generate pointwise
   F/R propagation and use the imported `tick_block()` wrapper for replay.
   Preserve semantic SCCs separately from the expanded background evaluation DAG,
   enforce the recorder boundary and keep Tock execution off the audio thread.
4. **In progress: storage-requirement inference and ordinary background evaluation.**
   Exact source/target port-atom partitioning, independent capability joins,
   and immutable storage planning have landed. The storage plan selects
   canonical persisted-data bindings, current-Tick views, materialized windows,
   transaction-local materializations, direct channel/event views, and exactly keyed
   shared derived conversion/fan-in operations. Materialized addressable derived storage
   subsumes its otherwise-identical materialized sequential form. Logical callback/
   coverage ports remain port-granular. The current monolithic executor substrate
   now owns active/pending compiled realizations, stages pending storage without
   reading live mutable state, performs ordinary `NodeStorage` migration only at
   an explicit quiescent boundary, and dispatches the already-active generated root
   without hidden lifecycle work. Each realization now owns an instance of the
   standalone reusable `BackgroundPropagationWorkspace`: generation-local port/node
   indices bind the compiler-planned accumulators into `BackgroundEvaluationCall`,
   exact fan-in changes and fan-out requirements converge before one generated F/R
   callback per implicated node, and generated pointwise replay propagation uses the
   same frame. Successful preparation exposes exact changes/requirements and a
   node-activity snapshot with `evaluate` selected for reverse-demanded nodes. No
   public executor operation commits that candidate after F/R alone. The complete
   transaction promotes prepared coverage only at final transaction commit, together
   with retained-data-root publication when a candidate is present.

   Complete this checkpoint in the following dependency order:

   1. **Landed:** extract the reusable propagation machinery as a standalone runtime
      component, separate committed coverage state from it, introduce
      prepare/commit/discard semantics, and remove the interim propagation-only
      executor API;
   2. **Landed:** retain immutable per-node materialization placement, runtime
      binding-slot maps plus replay invocation constraints/schedule slots in the
      compiled plan, with validation;
   3. **Implemented; build/test verification pending:** replace the executor-level
      fixed-width page prototype with the canonical `PersistedDataStore`, private
      candidates, one immutable published
      sample/event root, stale-base checks and audio-safe reader pins. The root maps
      compact output handles through a path-copied directory to per-output path-copied
      range trees. Samples are immutable contiguous `PersistedSampleRegion` records;
      events are explicit-range `PersistedEventChunk` records, including authoritative
      empty chunks. Candidates structurally share unchanged records/subtrees, remain
      private until whole-root publication, and carry semantic/data coordinates; a second candidate
      from a superseded root is rejected without changing the publication. Stable
      output identities and explicit generation-local identities use the same store.
      Reader slots are registered off the audio thread; pin acquire/release performs
      only bounded atomic operations and holds a non-owning root pointer. Superseded
      owning roots enter a retired list and are destroyed only by an explicit
      non-audio reclamation pass after no slot pins them. Root-block-size changes do
      not repartition retained data;
   4. **Landed:** add address-stable runtime views and small sample/event
      direct, conversion, projection, fan-in and merge operations, then realize
      the complete `BackgroundStoragePlan` for transaction-local and persisted
      storage. `BackgroundStorageRealization`
      resolves plan storage indices into stable opaque sample/event read/write views,
      owns sparse transaction-local and future materialized-window buffers, binds
      non-owning published-data readers under the surrounding transaction's pin,
      derives stable or generation-local persisted identities outside the data store,
      and seals only after required views are compatible. It now also executes each
      compiler-retained runtime operation at most once against those sealed views.
      Direct sample/event operations remain copy-free alias validation; sample
      materialization applies per-source read latency plus the planned heterogeneous
      conversion/projection assembly; event materialization applies the immutable
      non-expanding conversion and reuses the shared stable k-way merge, preserving
      `(range-relative time, semantic source order)`. It still does not traverse nodes,
      decide demand or publish. The generated hook frame selects
      the already-placed operation indices; the realization never rebuilds placement;
   5. **Landed:** use the generated background root's narrow
      transaction-supplied prepare/finalize hooks and the populated Tock/replay
      call-frame owner to implement the complete transaction coordinator. The
      generated evaluate root now loads a prepare hook,
      finalize hook and one opaque operation-frame pointer from each active node frame;
      it calls prepare immediately before authored Tock or the complete replay loop and
      finalize only after that invocation returns successfully. Forward/reverse roots
      do not call storage hooks. `BackgroundStorageOperationFrame` retains only the
      realization plus the immutable before/after operation spans for that node and
      executes them in compiler order; generated code receives no executor, store or
      transaction pointer. `BackgroundEvaluationCallFrame` now owns those stable
      operation frames, resolves compiler binding slots into authored-Tock facades,
      reserves typed replay binding arrays and validates replay regions against the
      compiled block limit. The propagation machinery is now extracted from
      the monolithic executor into a standalone workspace, and its move-only prepared result
      exposes exact requirements plus immutable node activity for the coordinator;
      reverse-demanded nodes are marked for evaluation only after successful F/R.
      `BackgroundEvaluationTransaction` now pins exactly one published base, lets a
      narrow post-forward demand policy complete exact stale or missing persisted ranges
      before reverse propagation, derives logical-binding and physical-storage
      selections, and owns a private persisted-data candidate only when retained state
      actually changes. The call-frame owner allocates
      isolated power-of-two replay sample/event storage, populates inputs after the
      compiler-placed prepare operations, flushes outputs before the placed finalize
      operations, and binds no live realtime state. A replay node with several outputs
      flushes each output only over that output binding's selected coverage, even
      though the imported Tick wrapper runs over the union schedule. The transaction
      first seals every pinned persisted input only after proving that its selected
      coverage is present in compatible published data. It then invokes the generated
      evaluate root once with the prepared activity and
      verifies that every selected produced sample/channel was initialized and that
      no sample/event callback write was rejected before staging any record. Failure,
      including incomplete sample production, discards all
      transaction-local values; stale-base rejection promotes neither data nor
      semantic coverage. A transaction that produces no persisted data and no
      invalidation does not advance the data version, but still preserves the pinned
      base's semantic-version monotonicity and retained-layout compatibility and
      revalidates that base as current before promoting semantic coverage. When retained
      data does change, successful publication precedes the propagation workspace's
      non-throwing coverage promotion. `BackgroundExecutor`
      exposes this end-to-end operation and no propagation-only compatibility API; and
   6. **Complete:** Tick-time dynamic bindings use
      `TickMaterializationSnapshot` playback and per-input neutral values for genuinely
      missing sequential data. The generated Tick root's final ABI accepts one
      `TickInvocationCall` containing only resolved sequential and Random Access view
      spans. `RealtimeExecutor` registers both reader slots on the control path and constructs
      a non-copyable callback-scoped `TickInvocationFrame`; that owner acquires one
      bounded atomic persisted-data-root pin and retains it across the complete generated-root
      invocation. Generated code receives neither that owner nor a persisted-data-store/executor
      pointer. Immutable compiler slot maps now assign each node's dynamic Sequential
      and Random Access sample/event inputs contiguous kind-specific ranges in the
      invocation record. Sequential slots retain persisted-data and
      Tick-materialization candidates; generated-root current-Tick composites remain
      operations rather than external binding slots. Sample neutral values remain
      properties of their logical input ports.
      A realization-owned,
      address-stable `TickInvocationWorkspace` resolves persisted identities on the
      control path. Random Access callback binding only retargets its preallocated
      views to the pinned root, while Sequential sample binding refreshes its bounded
      preallocated rings. Published snapshots precompute exact per-output coverage metadata,
      so direct identity sample-region/event-chunk views require no callback-time allocation or
      coverage construction; generated lowering passes the corresponding node-local
      subspans to imported Tick wrappers. Successful background transactions now also
      freeze compiler-routed derived, converted, fan-in and ephemeral Random Access
      results, plus direct background-only Sequential materializations, into one
      immutable `TickMaterializationSnapshot`. Mixed `current_tick` composites remain
      generated-root work because they also consume live Tick sources. Its promotion is a
      no-fail owner relink after any persisted-data publication and before semantic coverage
      promotion. Background then publishes a mailbox root that pins that materialization
      together with its exact persisted-data root. Realtime adopts the pair at a pass boundary
      without reference counting; retired pair owners are reclaimed only by an explicit
      non-audio executor operation. Each snapshot records its compiled generation and
      exact persisted-data version, and an incoherent pair is rejected before mailbox
      publication rather than repaired inside a callback. Sequential sample slots own
      preallocated buffers sized from the specialization's maximum block size. The
      callback fills only the root invocation's current window from coherent
      materialization or direct persisted-region data, and leaves absent frames at
      that logical input's neutral value, without allocation. Sequential event slots
      likewise own preallocated sequences sized from the selected storage's aggregate
      event rate and maximum block size. Callback binding resets the count and copies
      only events in that window; missing data is the empty sequence. A consumer with
      history keeps it in its own node-owned timeline, which the generated root
      extends by one window per callback (status 2026-10-23; the superseded slot
      sizing is retained in [Background-fed input playback
      history](./historical/background_fed_input_playback_history_2026-10-23.md)).
      Generated lowering now overlays each node's compact
      Sequential slots onto a stack copy of its complete ordinary input-binding array,
      preserving unaffected and mixed live inputs. Sequential sample and event spans
      are both supplied by callback binding.

   Final commit atomically promotes prepared semantic coverage plus any candidate persisted-data
   publication. Step 5 below extends that same boundary with selected
   realtime-produced queue prefixes. Any failure or stale-base rejection promotes none of
   the state owned by the current transaction. Expose only the end-to-end semantic transaction operation;
   do not add a public F/R-only commit path. Verify throughout that no
   audio-thread path invokes Tock, allocates, blocks or reclaims the final owner of a
   retired snapshot. This checkpoint does not add a `ProjectGraph` or application-
   module bridge; that wiring follows only after the executor transaction boundary is
   complete and tested.
5. **Tick/persisted recording transport and disposition semantics landed.**
   Realtime-produced persistence uses producer reserves and background pending queues,
   as described normatively in
   [realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md).
   The old capture store, capture-output registry, global capture sequence/frontier
   and `TickInvocationWorkspace` compatibility adapter have been deleted.

   Keep one `GraphExecutor` app module with internal `RealtimeExecutor` and
   `BackgroundExecutor` actors and no shared mutable execution-state object. Prepare one
   complete `ExecutionGeneration` off-thread: both realizations, routes/endpoints,
   producer reserve requirements, pre-sized background input descriptors, migration
   metadata and cutover linkage must be ready before its pointer can become pending.

   Producer-specific
   `ProducerReserve`s maintained by `AsyncCapacityManager` and producer-specific
   background `PendingQueue`s. Producers supply `C/L/H`; allocation/slab granularity is
   manager-owned. Where layout permits, realtime writes final sample/event payload
   directly into reserve blocks, builds chains privately, then publishes `(first,last)`
   at a pass boundary with one cheap pending-queue attachment.

   `BackgroundExecutor` independently pins one finite `(first,last)` prefix per queue
   into pre-sized descriptors. There is **no** atomic cross-queue snapshot. Once
   selected, the workload is immutable; later appends or generation/control changes are
   next-pass work. If a future feature requires atomic visibility across queues, stop
   and design it explicitly.

   On coherent commit, apply Tick/persisted recording payloads, publish the successor
   retained state/data version, and release consumed blocks. One background worker may
   return completed blocks from all producer queues through one SPSC released-block
   stream to `AsyncCapacityManager`.

   Background-to-realtime persisted-state delivery is a latest-version mailbox, not a
   queue: if several compatible immutable versions are ready before realtime reaches a
   pass boundary, only the newest pending version needs to survive. Superseded pending
   versions are reclaimed off realtime.

   Preserve fixed recording semantics while migrating transport: `untouched` publishes
   nothing and preserves prior RAM recording; ordinary writes overwrite their addressed
   range; `write_void()` publishes an explicit erase. Resource exhaustion makes
   mandatory recording/persistence incomplete and never authorizes intentional dropping.
   An explicitly authored zero-event Tick/persisted block remains authoritative empty
   event data, not a recording void.

   Statically specialized Tick/persisted sample/event output accessors implement
   invocation-local `OutputDisposition`, and reflected output bindings retain the
   finalized value for generated post-step operations. Ordinary ephemeral/Tock writes
   do not update disposition. An explicit empty event-block write marks `written`;
   omitting output authoring leaves the block `untouched`. The queue producer and
   background transaction translate those finalized dispositions into payload,
   omission, or coverage erasure in canonical retained data.

   Any remaining Tick/persisted recording work must reuse this transport. It must not
   recreate a recording-specific allocator/log, global capture insertion
   sequence/frontier, shared logical queue size, separate cutover allocation, or
   alternate `TickInvocationWorkspace` construction path. Keep explicit domain
   ordering or generation/version identities only where semantics require them.

6. **In progress: implement concrete-node port-state continuity and graph-revision transitions.**
   Graph-wide connection analysis now retains each authored concrete node's optional
   stable graph/virtual-node/direct-member identity, and background planning consumes
   that same identity instead of reconstructing a background-only copy. Anonymous
   concrete nodes deliberately retain no synthetic identity derived from a
   generation-local bundle handle.

   Connection analysis also now derives the authoritative inventory of non-empty
   node-owned Sequential-input history and Tick-output history/latency. Sample state
   is identified per semantic channel; event state is identified per stream; history
   and authored-future latency are distinct roles. _(Since 2026-11-07, a sample output
   has one `tick_output_history` role covering its history + latency values, all of
   which lie before its next tick; only an event output has a separate
   `tick_output_latency` role. See Front 16 in implementation_fronts_direction.md.)_ Every entry retains its exact extent
   and generation-local port coordinate, while entries for authored stable nodes also
   retain graph/virtual-node/direct-member, direction, port kind/name/index, channel or
   event-stream marker, and role. `CompiledGraph` retains this inventory as cold
   metadata; generated realtime execution pays no lookup or disposition cost for it.

   Sample-state inventory entries are now mapped after final `NodeLayout` placement to
   separate callback-facing and cross-callback views: immutable constant, callback-local
   arena view, compact cross-callback carry, or persistent ring. The cold mapping retains
   the signed semantic-to-storage timeline offset (`P + offset`), working capacity,
   stored-frame count, compact-carry future extent, and finalized `NodeStorage` or
   callback-arena-relative byte location. For an explicitly materialized input it also
   records every retained source-layout channel and the exact source/target window needed
   to reconstruct the callback channel. These are generation-local realization facts and
   are never part of semantic identity or the realtime node API. Unresolved composition
   and callback-only views remain explicitly non-migratable after the root returns and
   require transition-only storage rather than reads from expired arena bytes.

   Event-state inventory entries are now also mapped after finalized placement. The cold
   mapping records the callback representation separately from the cross-callback
   representation, resolves compact-carry working sequences to their persistent backing,
   and carries finalized event/header/source-index offsets. Direct streams,
   retained-source materializations, source-indexed merged streams, immutable empty
   streams, shared unindexed merged streams, and callback-only streams remain distinct.
   A source-indexed merged stream retains its semantic source ordinal; a shared unindexed
   stream is never mistaken for one producer's state. Retained fan-in now allocates the
   source-index sidecar precisely when a source owns Tick-output history or latency;
   target-history-only fan-in stays unindexed. Disconnected Tick event outputs with their
   own history/latency now receive the same compact-carry or persistent-ring backing as
   connected outputs, while zero-retention disconnected outputs remain callback-local.
   Materialization names a storage operation rather than necessarily a type conversion: a
   same-type retained fan-in may have a zero-step conversion plan while still
   materializing the selected invocation window for its consumer.

   The next migration substep resolves multi-source sample compositions and audits any
   remaining callback-only realization that owns semantic state. It must trace derivations
   where possible and otherwise plan private transition retention. Then old/new stable
   port-state identities can be compared and their overlapping temporal ranges assigned
   direct migration or a finite transition realization.

   Before optimization, define port history/latency exactly as if each surviving
   concrete node privately owned that state. Carry stable user-instance/virtual-member/
   port/channel-or-event identities plus cold realization metadata; preserve the
   overlapping valid temporal range across rewiring, fan-in/fanout changes, size
   changes, and compact/ring/alias representation changes. When the steady new graph
   cannot directly represent inherited state, compile a transition realization with
   temporary materialized state and a finite absolute-position expiry horizon, plus
   the final steady realization. `RealtimeExecutor` performs the splice and the later
   safe-boundary handoff without requiring another compilation. This stage is a
   correctness prerequisite and does not require first simplifying `NodeStorage`.
7. **Finish generation reconciliation and remaining authored-node cases.** Rebind
   compatible persisted output stores and finish remaining nested-state, feedback,
   event, detach, activity/TTL and skip semantics. Preserve persisted generated/
   finalized data throughout its covered lifetime; coverage removal remains the only
   semantic deletion condition for persisted output data.
8. **Add first-class scalar/batch callback normalization and scheduling.** Add the
   batch context ranges and complete `do_*` scalar/batch helper pairs for Tick, skip,
   Tock and forward/reverse propagation; retain both callback anchors in package LLVM;
   classify compatible concrete realizations; and let Tick/background schedulers group
   ready same-class nodes while retaining scalar fallback. Treat virtual-node member
   grouping as a discovery hint, not the only batching boundary. The detailed API and
   invariants are in
   [Batched Node Callback Direction](./batched_node_callbacks_direction.md).
9. **Optimize only after transition correctness is established.** Refine batch sizing,
   cross-node SIMD, fusion, retained-record placement, transient reuse, storage aliasing/liveness
   and immutable-value specialization only after graph-version state continuity is
   covered by tests.

## 33. Summary invariants

1. Input access is Sequential/Region/Coverage. Tick output production is
   Sequential; Tock output production is Region/Coverage. Both share
   `computed`/`buffered` realization.
2. A computed output provides computation, not proportional storage or full
   logical Region buffer pointers. A buffered output provides independently
   accessible data, but neither mode forces eager computation or cache policy.
3. Coverage outputs never feed Region inputs. Complete-buffer Region access
   needs a buffered Region source (or an explicit bounded materialization
   node). Windowed Region/Coverage access preserves logical Region identity.
4. Exact `IndexCoverage`, logical Region decomposition, reverse demand and
   already-materialized positions are independent. The executor computes only
   what consumers and actual dependencies require.
5. Published buffered snapshots can be partially materialized. Every
   promised requested window is complete, pinned and version-consistent.
   Uncomputed covered positions are not gaps; missing realtime preparation
   is an underrun, not Sequential fallback.
6. The executor controls caching and must retain or reconstruct all semantic
   information required by selected snapshots. Recording is a normal
   Tick/Sequential/buffered output, not a third category.
7. Tick and Tock output shapes are disjoint; realization modes are shared.
   Tick Sequential production is continuous and its `.detach()` feedback history
   remains non-replayable; Tock and coverage/dependency propagation remain
   on background workers. Realtime consumers use prepared bounded windows.
8. Contextual Tick replay, where supported, requires deterministic and
   acyclic dependencies or a published recorded boundary, independently of
   output shape and realization.
9. The existing static graph and storage analysis, queue publication, Tock
   binding, coherence and change-propagation requirements remain, except
   where superseded explicitly by
   [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md).

## Sparse Sample events and replayed curve readers (2026-11-13)

`SampleEvent` carries one Sample value at its `TimedEvent::time` position.
Holding and interpolation are consumer policies. The event type does not impose
a curve shape or a persistent point ID.

`EventRegion::neighbors(position)` returns the last event at or before a position
and the first strictly later event in that same logical Region. Equal-position
events preserve their semantic order. Sorted storage supports binary search
relative to the Region's beginning, including across circular index seams.
Neighbor search must not borrow a point from another overlapping Region.

A sparse Tock source can provide a complete point Region to a replayable Tick
interpreter. The interpreter's authored forward callback publishes positional
`IndexCoverage` for its Tick output; its reverse callback requests the point
Region needed to interpret the demanded output positions. This does not change
the output's Tick production into Region or Coverage production. Replayed Tick
sample storage follows the demanded positions rather than copying the source's
entire availability range. Authored Tock Regions and recorded Regions keep their
complete membership and storage.

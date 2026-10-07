# Coverage, Random Access, And Background Evaluation

[DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md) defines the normative vocabulary used here.


> **Planned random-access representation change:**
> [Random-Access Port Data, Audio Value Types, And Input Contract Direction](./random_access_port_data_direction.md)
> supersedes this document where it requires disjoint/canonical `Coverage` regions,
> page-backed random-access sample storage, or page-oriented node access. The target
> API uses contiguous Region/Coverage sample views; Coverage regions may overlap. The
> page-oriented sections below remain useful as the current implementation checkpoint
> until that migration lands.

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
node's `Coverage`. Every batch lane retains its own exact coverage/state/context, and a
one-lane batch is always legal.

Node authors choose one supported scalar-or-batch implementation shape for an
operation; the traits layer derives both normalized scalar and batch operations. The
background scheduler may then use ready-frontier batching without changing the
transaction's coverage semantics.

The central rules are:

> Input access, output production, and output retention are independent authored
> contracts. `SequentialInputConfig` declares a bounded sequential read window;
> `RandomAccessInputConfig` declares arbitrary reads within available exact coverage.
> `TickOutputConfig` selects sequential production by the existing generated
> `tick_block()` implementation; `TockOutputConfig` selects demand-driven production
> by `tock_coverage()`. Either output may be `ephemeral` or `persisted`.

> All concrete GraphJit node types have static constexpr port schemas. Graph
> topology, node instance count, and connections remain dynamic graph data.
> Replayability is a separately declared and validated **node trait**, not a new
> `tick()` contract, port field, or alternate DSP callback. Contextual replayability
> additionally requires every upstream value for the requested coverage to be
> available or reproducible.

> `tock_coverage()` and its propagation/evaluation transactions run **only off the
> audio thread**. An audio-thread sequential input reads the currently published
> page even when it is out of date; for a missing page it supplies that particular
> input's `neutral_value`. No cache miss invokes tock or blocks the audio thread.

> A random-access input may consume a Tock output (through persisted pages or an
> ephemeral addressable materialization), a Tick/persisted output's published pages,
> or a contextually replayable Tick output. An unreproducible Tick/ephemeral output
> cannot directly satisfy random-access demand: the DSP author must select persistence
> or place an explicit recording node at the authored boundary. The restriction
> prevents **implicit recording**, not a technically impossible connection. The
> preliminary Tick-time Random Access path reads only a pinned published/materialized
> immutable view; it never aliases current mutable Tick output.

> Tiling is a non-materializing composition of source channels. It preserves each
> source's production, retention, and effective access capabilities. A whole-tile
> random-access connection requires every selected source channel to satisfy the
> requested random-access contract; channel projection retains its own capabilities.

> Computed tock outputs publish exact finite `Coverage`. Forward propagation keeps
> output coverage exact but may conservatively over-report regions whose values could
> have changed; reverse propagation may likewise conservatively over-request input
> regions. Under-reporting either dependency direction is a correctness error, while
> over-reporting is a performance cost. Page boundaries never widen semantic coverage
> or the already-reported forward may-change regions. Mechanically pointwise replayable
> Tick dependencies may still use compiler-generated propagation.

> Invalidation and requested coverage are processed as batched background
> evaluation transactions: all roots accumulate
> before traversal; fan-in/fan-out requirements are unioned; each applicable authored
> forward/reverse/tock callback runs at most once per implicated node per transaction.
> `TockState` remains non-semantic acceleration available only to tock.

> No unresolved random-access computation dependency may participate in a cycle.
> Replayable tick subgraphs must be acyclic when expanded for background demand;
> valid published persisted values can terminate replay traversal. Ordinary
> Tick-to-Sequential feedback retains its existing scheduling semantics.

> Persisted output pages are **never evicted** for age, cache limits, memory
> pressure, or invalidation. Retained generated pages may be removed only when
> output coverage no longer includes them; replacement storage versions are
> reclaimed only after their readers release them. Unbounded memory use is an
> explicit consequence of the author's persistence declaration.

> A Tick/persisted output is the recording output; there is no third retention mode or
> separate recorder-port marker. An explicit recording node is an ordinary authored
> node that consumes the live Sequential input and writes a Tick/persisted output. It
> uses already-provisioned producer-reserve storage for each block actually written to
> that output. Leaving the output untouched preserves
> previously recorded RAM data at that timeline position; `write_void()` publishes an
> authoritative erasure. Each background pass independently pins one finite prefix
> from every relevant producer queue; selected changes enter ordinary exact
> invalidation, reverse planning, background computation, and coherent publication.
> Pending queue storage is transaction input, not the recorder's Random Access
> representation.

This is an incremental coverage-evaluation model integrated with Tick execution,
not a second audio-thread scheduler and not a storage class.

## 1. Port model

Port data/value type, input access, output production, and output retention
are independent. The callback that *uses* an input does not determine that input's
access: a `tick_block()` implementation can consume random-access inputs, including
when that Tick implementation is replayed in the background.

| Axis | Authored alternatives | Meaning |
| --- | --- | --- |
| input access | `SequentialInputConfig`, `RandomAccessInputConfig` | bounded sequential window or arbitrary positions inside coverage |
| output production | `TickOutputConfig`, `TockOutputConfig` | existing generated tick-block implementation or background tock callback |
| output retention | `OutputRetention::{ephemeral,persisted}` | whether generated finalized values must be retained |
| data | registered continuous value type, event type | value/event representation and bounds |

Sequential input history and tick output history/latency are finite timing
contracts. Random-access input and tock output declarations do not acquire these
fields. Output retention never selects a callback. The concrete sample/event
schema is in section 13; the direct-connection rule is in section 22.

`tick()` already exists: the node traits generate its `tick_block()` wrapper, and
GraphJit imports that wrapper's LLVM definition. The new replayability trait
permits **some** such nodes to execute through that same wrapper in the background
evaluation DAG. It does not make all `tick()` implementations pure or replayable.
No `tock_coverage()` call occurs on the audio thread.

## 2. `Coverage` is the only coverage boundary

Every tock output publishes a finite **coverage**: a canonical union of
nonempty, disjoint half-open global index regions. There is no separate `extent`,
bounding hull, or implicit infinite default-valued domain.

For example:

```text
[0, 480000) U [4800000, 5280000)
```

may describe two audio clips separated by a large empty timeline gap. The gap
requires no persisted-page storage, validity metadata, reverse demand, or Tock
work.

Outside coverage, the output is not requestable by node code. The executor and
node wrappers should assert this contract in debug builds. This is stronger than
saying that reads outside coverage return zero/no-events: lowering is allowed to
assume such reads never occur and optimize accordingly.

Disconnected random-access inputs have empty coverage.

A random-access input exposes the canonical union of the mapped, available
coverages of its connected sources, including tock outputs, finalized published
persisted tick outputs, and contextually replayable tick outputs. Both
`tick_block()` and `tock_coverage()` may inspect such input coverage. The
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

## 3. `Coverage` helper

Node and runtime code should use one small helper value type rather than manually
maintaining vectors of regions.

Conceptually:

```cpp
struct IndexRegion {
    SampleIndex begin;
    SampleIndex end; // half-open [begin, end)
};

class Coverage {
public:
    using Region = IndexRegion;

    Coverage() = default;
    explicit Coverage(Region);
    explicit Coverage(std::span<Region const>);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(SampleIndex) const noexcept;
    [[nodiscard]] bool contains(Region) const noexcept;
    [[nodiscard]] bool intersects(Region) const noexcept;

    void include(Region);
    void include(Coverage const&);
    void exclude(Region);
    void exclude(Coverage const&);

    [[nodiscard]] Coverage intersection(Coverage const&) const;
    [[nodiscard]] Coverage difference(Coverage const&) const;

    auto begin() const noexcept; // region iterator
    auto end() const noexcept;   // region iterator
};

Coverage operator|(Coverage const&, Coverage const&);
Coverage operator&(Coverage const&, Coverage const&);
Coverage operator-(Coverage const&, Coverage const&);
```

The exact API may evolve, but the canonical representation invariant is fixed:

- every stored region is nonempty: `begin < end`;
- regions are sorted by global index;
- regions never overlap; and
- adjacent regions are coalesced, so consecutive stored regions satisfy
  `previous.end < next.begin`.

Thus `[0,10)` and `[10,20)` canonically become `[0,20)`. There is exactly one
canonical region sequence for a given coverage set.

`begin()` / `end()` iterate **regions**, not sample indices. Deliberately do not
provide a convenience bounding `extent()`, `first_index()`, or `last_index()` API
on this type: making sparse coverage explicit helps prevent accidental work over
long uncovered gaps.

`include()` / `exclude()` maintain canonical form. Construction from many
regions should sort once and merge once rather than repeatedly performing
quadratic insertion. The initial backing storage may simply be `std::vector`;
encapsulation leaves room for a later small-inline representation without
changing node-facing code.

Useful transformations such as shifting or finite support expansion may remain
free functions rather than growing the core container API:

```cpp
Coverage shifted(Coverage const&, SampleIndex offset);
Coverage expanded(
    Coverage const&, SampleIndex before, SampleIndex after);
```

The same type represents exact output coverage, semantic may-change regions, and
reverse may-read requirements. Only output coverage is required to be minimal:
forward invalidation and reverse requirements are sound upper bounds. Cache validity
is deliberately coarser and is not represented by one `Coverage` bit per sample.

## 4. Coverage, stored page domains, validity, and data

The executor distinguishes several concepts for persisted outputs:

- **coverage**: exact regions where the output exists and may legally be requested;
- **page domain**: for one stored page, `page_interval & coverage`;
- **validity**: whether that whole page domain is current for a candidate
  `(semantic_version, page_version)` pair;
- **data**: retained sample/event values for the page domain;
- **semantic version**: the dependency/configuration environment for which those
  values are meaningful; and
- **page version**: the immutable published page view against which those values
  were computed or read.

Coverage is exact and independent of page boundaries. Stored pages for
persisted outputs use the same canonical quantum as the fixed whole-graph root block for the
active executable/layout generation. For root block/page width `1024` and output
coverage:

```text
{ [10, 1002) }
```

page zero has the semantic page domain:

```text
[0, 1024) & coverage = { [10, 1002) }
```

Node code never observes `[0,10)` or `[1002,1024)` merely because stored data is
page based.

A page has one validity state for one candidate version. It is not partially
valid. If its page domain is:

```text
{ [10,100), [500,600) }
```

then a valid page means that **both** covered regions are current. An invalid page
means none of that page domain may be used as current candidate data until the
page domain is rematerialized successfully.

For a published `tock/persisted` output, every page with a nonempty page domain is
valid: the entire published coverage is materialized. Partial page validity is
therefore an internal candidate-building state, not an externally observable
published-output state.

RAM placement is intentionally not part of coverage or retention semantics. A complete
stored output may later use ordinary heap memory, stable arenas, mmap-backed files,
compressed backing, or another representation. Such choices must still satisfy
the audio-thread read requirements of the generated graph, for example by pinning or
prefaulting live regions where the backing mechanism can otherwise fault or block.
They do not reintroduce semantic "missing pages" into a published stored output.

## 5. Sparse logical requests and stored materialization

External random-access requests may be sparse. UI waveform rendering may ask for a few
positions from a very large coverage, for example. Sparse demand behaves according to the output's access and retention semantics, not according to a generic cache policy.

A `tock/ephemeral` output has no persisted materialization obligation. A sparse request is
intersected with exact coverage and stays exact through that output. Its
`tock_coverage()` work may therefore compute only the requested covered regions,
writing directly into caller/result or transaction-local storage.

A published `tock/persisted` output is different: the whole output coverage has
already been materialized before publication. A sparse external request merely
reads the requested subset from that complete stored representation. It does not
cause the executor to create a partially populated stored output.

Page granularity still matters while constructing a **candidate** stored version.
If a reported semantic may-change region intersects one or more stored pages, those page domains
become invalid in the candidate. Rebuilding the candidate selects the complete
covered domains of every invalid page:

```text
materialization(page) = page_interval & candidate_output.coverage
```

For `P = 1024`:

```text
coverage = { [10,100), [500,600) }
changed  = { [20,21) }

=> candidate page 0 becomes invalid
=> rematerialize { [10,100), [500,600) }
```

The uncovered gap is never processed. Once every nonempty page domain of the
candidate is valid, the `tock/persisted` output is complete for its target
`(semantic_version, page_version)` pair and may become publishable.

A tick/persisted output retains finalized sequentially produced values; its
published coverage is a random-access **stored boundary** without requiring tock
or an additional recording node. A replayable tick/ephemeral output can instead
satisfy random-access demand by recomputing from available upstream values. Only
an unreproducible tick/ephemeral source requires explicit recording before such
demand. Semantic changes to any published retained source start downstream sound
invalidation; they do not erase old readable pages.

Thus stored-page width is **not** an independent persisted-page storage tuning knob. It is
the active whole-graph root block size. Choosing a smaller or larger root block
therefore changes both Tick scheduling granularity and stored-page granularity,
while coverage and external-request semantics remain unchanged.

## 6. Dependency propagation remains independent of page granularity

Page granularity is a stored-materialization decision only. Semantic dependency
propagation remains expressed in `Coverage` region space. Output existence coverage is
exact. Forward may-change regions and reverse may-read requirements may be conservative
supersets when an exact dependency footprint is unavailable or too expensive to derive.

If a mutation semantically changes:

```text
[523,530)
```

an exact node may report `[523,530)` downstream. A less precise node may report a
larger sound superset. If the reported region
intersects a `tock/persisted` candidate page, the whole page becomes invalid locally,
but the executor does **not** widen the already-reported semantic may-change region to
the whole page merely because of storage granularity.

When the executor later rebuilds that stored candidate, it selects the complete
covered page domain before reverse propagation through the producer because the
producer is committed to materializing that entire page domain:

```text
reported semantic may-change region
        |
        v
invalidate touched stored page(s)
        |
        v
candidate completion selects full page_domain
        |
        v
propagate_reverse_coverage(producer)
```

A `tock/ephemeral` request has no such page promotion: its exact covered demand is
reverse-propagated directly, though each node's resulting input requirements may be a
conservative superset.

The important distinction is:

- forward semantic change may be conservatively widened by dependency semantics, but
  is never widened merely because a stored page becomes invalid;
- candidate completion for `tock/persisted` may page-promote work because the stored
  version must become complete; and
- requested output demand through `tock/ephemeral` remains exact because no retained
  output page is being completed, while its reverse input requirement may still be a
  conservative upper bound.

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
   recorded global positions. Queue insertion itself is not persisted-page
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

A forward update may change two independent things for each output participating
in background coverage propagation:

1. the output's new exact `Coverage`; and
2. a sound region superset inside the old/new coverage containing every value that
   may have changed.

The executor owns the previous coverage, so node code publishes the new coverage
as a whole value. The executor derives:

```text
added   = new_coverage - old_coverage
removed = old_coverage - new_coverage
changed = reported_value_changes | added | removed
```

Added or removed coverage is itself semantic change and propagates downstream.
For `tock/persisted`, every candidate page intersecting `changed`, an added/removed
page domain, or another representation-invalidating change becomes invalid in the
candidate. The reported `changed` set continues downstream unchanged by page
boundaries. It need not be minimal, but it must contain every actually affected
output position.

The forward phase does not recursively call `tock_coverage()`. It produces the
batch's semantic may-change regions and invalid stored-page set. The same logical batch
may then use those invalid page domains as materialization roots for a later
reverse/evaluation phase before publication; the phases remain distinct even when
the executor runs them back-to-back.

## 8. `propagate_forward_coverage()`

The forward dependency callback answers:

> Given this node's current random-access-input coverages, reported changed
> random-access-input regions, current semantic configuration, sample rate, and any
> local semantic change cause, what are the resulting exact computed-output coverages
> and what sound output-region superset contains every value that may have changed?

Conceptually:

```text
F(node): input coverage + changed input regions + semantic configuration + sample rate
      -> output coverage + affected output regions
```

It runs in forward graph direction. Incoming may-change regions from all random-access
inputs and all invalidation roots belonging to the batch are accumulated/unioned
before the node is visited. **Each implicated node is visited at most once by the
forward phase of one background evaluation transaction.** A node may still have many disjoint changed
regions and multiple changed inputs in that one callback invocation.

For every node that declares a computed tock output, an explicit forward-coverage
implementation is **mandatory**. Under the planned batch API that implementation may
be authored in scalar or batched form and is normalized to both forms by the traits
layer. Coverage is semantic and exact;
there is no generally correct fallback that can invent it. A framework may still
generate trivial glue when exact coverage is mechanically declared by another
static facility, but it must not silently preserve old/empty coverage or substitute
a conservative superset.

A node's affected-output region may be conservative. Under-reporting is a correctness
error; over-reporting is legal and only increases invalidation/evaluation work. The
callback is value-blind in the same sense as reverse planning: it may use coverage,
semantic configuration, sample rate and deterministic structural metadata, but it
does not evaluate transported input values merely to discover a tighter dependency
footprint. If a data-dependent address mapping cannot be bounded more tightly from
structural information, reporting the complete potentially affected output coverage
is valid.

A replayable Tick node with only mechanically pointwise dependencies needs no authored
forward or reverse coverage callback: GraphJit generates the same-position temporal
mapping from its static input/output dependencies. A replayable Tick node with Random
Access dependencies whose footprint is not mechanically derivable participates in the
same propagation framework and must provide the required dependency mapping; those
callbacks remain background-only and do not add a Tock output. A tick-only node with
persisted output is the recorded boundary for finalized data; it does not acquire a
`tock_coverage()` implementation. A Tock/persisted output remains an ordinary computed
output with exact forward-coverage semantics and background `tock_coverage()`
materialization; it is not a realtime recording output.

`TockState` is **not** visible here. Forward dependency semantics must not vary
according to memoization or acceleration history.

Forward and reverse propagation are directional dependency queries, not inverse
functions.

For an FIR-like transform of support `L`, for example:

```text
input changed [a,b)
    -> output may have changed [a,b+L-1)
```

while reverse demand maps output requirements to a different input region.

## 9. `propagate_reverse_coverage()`

The reverse dependency callback answers:

> Given exact covered output regions that must be evaluated by this node in the
> background, which covered regions of its random-access inputs must be available
> to compute them?

Conceptually:

```text
R(node): requested output coverage + semantic configuration + sample rate
      -> required input coverage
```

It runs in reverse graph direction. Requirements reaching a node through multiple
downstream paths and demand roots are accumulated/unioned before the node is
visited. **Each implicated node is visited at most once by the reverse phase of
one background evaluation transaction.**

For `tock/ephemeral`, the output requirement is the exact covered demand. For
`tock/persisted`, candidate completion first selects the complete covered domains of
invalid stored pages, and those selected domains become the requirements supplied
to reverse planning.

Persisted output data is a reverse-propagation boundary only when it is valid
for the `(semantic_version, page_version)` pair selected by the batch:

- a valid `tock/persisted` page/domain for the selected target/base version pair
  satisfies that requirement and stops reverse propagation through that region;
- an invalid or nonexistent `tock/persisted` candidate page does **not** stop reverse
  propagation merely because an older stored page still exists. Its complete
  covered page domain becomes a materialization requirement and reverse planning
  continues through its producer; and
- `tock/ephemeral` owns no persisted result and therefore never forms a retained
  reverse cut.

Thus page retention and page semantic validity are deliberately distinct. Never
dropping old immutable page data does not make those data current for a
new semantic version.

After the callback reports input requirements, every requirement is clipped to the
input's exact coverage.

The callback is **value-blind**. It may inspect coverage, semantic node
configuration, sample rate, and other deterministic structural metadata, but it
must not inspect random-access input data values to discover a second-stage
dependency footprint. If exact addressing depends on a random-access control input,
the callback returns a conservative superset derivable without reading that
signal, up to the complete potentially relevant input coverage. A future staged
value-dependent dependency facility may relax this rule without changing the
initial ABI.

The callback may be omitted where a conservative rule is mechanically
known, for example by requiring all covered regions of every random-access input.
Output-only Tock sources have nothing upstream to request and therefore require
no reverse callback.

For a tock-produced output needed by sequential playback, reverse planning and
all required computation run off the audio thread. The audio thread only reads
the already published/materialized result; an unavailable page uses the sequential
consumer's `neutral_value`.

`TockState` is **not** visible here. Dependency requirements may not depend on
memoization history.

## 10. Tock coverage evaluation callbacks

`tock_coverage()` is the checked-in one-node background-evaluation callback for
outputs whose production is `TockOutputConfig`. The planned
`tock_coverage_batch()` form evaluates several compatible nodes as one compiler/runtime
operation; each lane still carries the exact scalar context for its own node. A
node's requested `Coverage` is never merged with another lane merely because the
nodes are batched.

Within one background evaluation transaction, reverse planning first finishes
accumulating the final requested coverage for every implicated output. Forward
evaluation then executes the normalized Tock operation **at most once per implicated
node for the whole transaction phase**, either as a scalar callback or as one lane of
a batch callback. Requests for several outputs, several disjoint regions, and several
downstream consumers are therefore coalesced before that node participates in the
operation.

The callback receives only work that needs computation for the target operation:

- a `tock/ephemeral` output receives exact demanded covered regions; and
- a `tock/persisted` output receives selected invalid candidate page domains, each
  already intersected with exact output coverage.

It never receives uncovered portions of a stored page.

The callback:

- sees random-access inputs and computed tock outputs;
- sees the project sample rate for the semantic version being evaluated;
- operates on covered global sample/event regions;
- cannot request random-access inputs outside their coverage;
- cannot depend on mutable or unavailable live tick values; finalized persisted
  tick outputs and contextually replayed tick values are valid sources through
  random-access inputs;
- cannot depend on sequential `State`;
- may use `TockState` solely as non-semantic acceleration/memoization state; and
- must produce observable results independent of background request order and the
  contents/history of `TockState`.

`TockState` has one deliberately narrow meaning:

> It may change the pace or implementation strategy of `tock_coverage()`, but it
> may not change output values, output coverage, reverse requirements, or any other
> observable semantics.

`TockState` is background-owned and is not part of packed realtime `NodeStorage`.
It may use ordinary dynamic allocation. A node may set up configuration/resource-
dependent acceleration data in `initialize()`, but anything derived from current
input contents or coverage belongs in the appropriate background `tock*()` work and
may be resized/recomputed there.

A correct node must therefore produce the same result from a freshly initialized
`TockState`. The executor is free to serialize access, duplicate state per worker,
discard/reinitialize it, or otherwise manage acceleration state without changing
semantics. One mutable `TockState` instance must not be concurrently mutated by
overlapping tock executions unless the node's own state representation makes that
safe. All tock execution is background-only. `BackgroundExecutor` may serialize,
duplicate, retain, move, or reset compatible acceleration state without introducing
any audio-thread lock, live callback, or ephemeral-output exception.

Successful tock completion is transactional. For samples, every requested covered
sample/channel of every requested output is completely initialized. For events,
the callback emits the complete event sequence for every requested covered region;
zero events is a complete result. Failed, cancelled, or superseded work commits no
partial output as valid.

Within one output invocation, events are emitted in nondecreasing global timestamp
order. Equal-timestamp producer-local order is preserved.

A single callback remains preferable to mandatory per-output callbacks because a
node may share useful work among several outputs. **Tock and its propagation
callbacks are background-only.** Ephemeral retention never grants an audio-thread
live-pull exception. Eligible pure `tick()` nodes are different: their already-
generated `tick_block()` implementation may execute during background replay.

The scalar name deliberately does not contain `batch`: `tock_coverage()` evaluates
one node. Executor-level transaction batching across demand roots is a separate
concept. The planned `tock_coverage_batch()` ABI evaluates **multiple compatible
nodes** simultaneously, with separate coverage, bindings, and optional acceleration
state for every lane; see
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

## 11. Batched requested coverage and stored-candidate completion

Background evaluation is organized around **batched demand**, not one traversal per
read or invalid page. A batch first collects all demand roots that are allowed to
participate in that transaction, unions convergent requirements, then runs one
reverse phase and one forward evaluation phase.

One background evaluation transaction is evaluated against one coherent environment:
one target/base `(semantic_version, page_version)` pair, one sample-rate/configuration
view, one coherent set of persisted-page bindings, and independently selected finite
prefixes from every relevant pending producer queue. A publication linked after a
queue's remembered terminal block is not part of that workload. There is no atomic
cross-queue snapshot requirement.
Pure stored reads may need neither R nor T; a pure invalidation
batch may defer R/T; and a mutation-plus-fetch operation may run F then R/T before
returning results. The batching invariant constrains how work is coalesced when a
phase is present, not which phases every caller must execute.

There are two closed classes of **external demand roots**:

1. **application/UI random-access fetches**, such as JSON-RPC requests for one or more
   requestable outputs/regions; and
2. **advance materialization demand** for sequential playback, scheduled in the
   background for the positions a `CompiledGraph::tick_block()` invocation may
   later read. Audio execution never synchronously turns a miss into a new demand
   transaction or runs tock.

The host protocol need not expose these as one-request/one-batch operations. One
application request may contain multiple fetches, and an update request may also
ask for results after the update. The executor normalizes the request into batched
mutation and demand roots.

There is also one important **internal materialization root**: every invalid
`tock/persisted` candidate page domain that must be completed before the target
version pair can publish. Internal page-completion roots use the same reverse
and tock machinery as explicit reads.

### Demand-driven access

For requested `tock/ephemeral` sink outputs, the batch:

1. intersects every explicit request with exact output coverage;
2. unions all covered requirements for each output;
3. reverse-propagates the coalesced requirements through ephemeral tock producers
   and contextually replayable tick nodes until reaching valid published persisted-output
   boundaries or source nodes;
4. evaluates implicated nodes in forward dependency order, at most once per node;
   and
5. returns/forwards the requested materialization from caller, transaction, direct
   consumer, an addressable transaction-local materialization for a random-access
   input, or materialized bounded transient storage for a sequential input.

A request for any published persisted output reads the requested subset from the
canonical persisted-page snapshot selected for the transaction. A Tock/persisted
candidate may require background reconstruction before a successor version can
publish. Tick/persisted data reaches the same page store through realtime-produced
provisioned-queue/finalization handoff and needs neither a Tock callback nor an
explicit recorder.

### `tock/persisted` candidate completion

Forward invalidation may create a candidate version with invalid stored page
domains. Before reverse planning, the executor promotes every invalid stored page
to its complete covered page domain:

```text
materialization_requirement(page) = page_interval & candidate_output.coverage
```

Those complete page domains are then unioned with all other materialization demands
for the same background evaluation transaction. To make the candidate publishable, the executor:

1. gathers all invalid/nonexistent `tock/persisted` page domains that the candidate
   requires;
2. unions those internal roots with any explicit demand roots allowed in the same
   transaction;
3. runs one reverse-order pass, coalescing downstream requirements before each
   node and stopping per-region at persisted output data valid for the selected semantic
   version;
4. runs one forward-order background evaluation pass: each implicated tock node
   gets at most one tock callback for the consolidated batch, while a replayable
   tick node runs its imported generated block wrapper over the required blocks; and
5. commits computed stored pages transactionally. A semantic candidate may publish
   only after every covered `tock/persisted` page/domain it owns is valid.

Conceptually:

```text
all invalidation roots for batch
        |
        v
one forward dependency pass (F)
        |
        v
sound may-change regions + invalid tock/persisted pages
        |
        v
promote invalid pages to complete covered page domains
        |
        +-------------------------------+
        |                               |
        |                    explicit UI/materialization demand roots
        |                               |
        +---------------+---------------+
                        |
                        v
              union all demand roots
                        |
                        v
one reverse dependency pass (R)
  each implicated node <= 1 reverse callback
                        |
                        v
one forward background evaluation pass (T)
  tock callback <= 1/node; replayable tick wrapper per required block
                        |
                        v
complete candidate stored pages / transient results
                        |
                        v
publish coherent candidate when all required stored state is complete
```

A valid published persisted page may satisfy an upstream requirement without
further traversal. An invalid Tock/persisted candidate page cannot: its older retained
data belongs to the previous semantic version, so its full covered page domain
remains in the transaction and its producer is traversed. Tick/persisted candidate
pages are filled from the fixed Tick-capture snapshot instead of traversing the live
Tick producer.

Complete stored publication does **not** require every transient intermediate or
all invalid pages to coexist in memory at once. `BackgroundExecutor` may reuse bounded
transaction storage while honoring the semantic batch. Such chunking must not
split a node into multiple `tock_coverage()` invocations for the same logical batch;
if workspace storage cannot hold the node's consolidated requirements, the
executor/lowering must provide a representation or bounded streaming contract that
still preserves the one-callback-per-node batch semantics.

## 12. Batched forward invalidation phase

A forward invalidation phase begins from the closed invalidation-root set in
section 7. All roots assigned to one background evaluation transaction are installed before
forward traversal starts.

The evaluator conceptually performs:

```text
all node-state / Tick-capture / graph / sample-rate invalidation roots
        |
        v
start from affected nodes/ports
        |
        v
walk nodes once in forward dependency order
        |
        v
union reported may-change input regions + current input coverage
        |
        v
propagate_forward_coverage() at most once for this node
        |
        +--> publish exact new computed-output coverage
        |       |
        |       `--> derive added/removed coverage as semantic changes
        |
        `--> publish sound may-change output regions
                |
                +--> invalidate intersecting tock/persisted candidate pages locally
                |
                `--> union semantic may-change regions into downstream accumulators
```

Fan-in never causes repeated callbacks within the batch. Every upstream path that
can reach a node in forward order has contributed to that node's per-input change
accumulator before its callback runs. Fan-out distributes the callback's consolidated
output changes into downstream accumulators.

Forward propagation continues **through** `tock/persisted` computed outputs. A stored
output is not an invalidation cut: keeping an older page data alive does not
make downstream semantics current for a new candidate version. Page boundaries
only decide which local candidate pages become invalid; they do not themselves widen
the semantic may-change regions already reported by dependency propagation.

The forward phase itself does not recursively evaluate nodes. After it finishes,
all invalid stored page domains are known and can be promoted to complete covered
page-domain demand roots for the batch's R/T phase. The executor may defer that
materialization, but a candidate containing `tock/persisted` outputs cannot become
published for its target version pair until all of its covered stored page domains
are valid.

## 13. Authored port schema, retention, and replayability

The port schema has independent input access, output production, and output
retention. Data properties remain separate for samples and events:

```cpp
struct SequentialInputConfig {};
struct RandomAccessInputConfig {};
using InputAccessConfig =
    std::variant<SequentialInputConfig, RandomAccessInputConfig>;

struct TickOutputConfig {};
struct TockOutputConfig {};
using OutputProductionConfig =
    std::variant<TickOutputConfig, TockOutputConfig>;

enum class OutputRetention { ephemeral, persisted };

struct InputConfig {
    // Existing sample/event data properties and name/identity also apply.
    InputAccessConfig access{SequentialInputConfig{}};
};
struct OutputConfig {
    // Existing sample/event data properties and name/identity also apply.
    OutputProductionConfig production{TickOutputConfig{}};
    OutputRetention retention = OutputRetention::ephemeral;
};
```

These are abbreviated target-schema sketches. The checked-in implementation still stores
Sequential history and Tick output history/latency in the static configs; the planned
constraint model moves those extents into per-instance `constrain_ports()` realization
constraints while retaining the access/production alternatives shown here. `InputConfig`
and `OutputConfig` retain their sample/event data properties and identities.
Input access, output production and output retention are independent in reflection,
configured-graph serialization and the compiler record interface. GraphJit classifies
per-channel compatibility and delivery using those orthogonal contracts; execution of
the retained background plan begins in step 4.

| production | retention | producer and retention semantics |
| --- | --- | --- |
| tick | ephemeral | sequential generation; no post-window retention obligation |
| tick | persisted | sequential generation; every written/voided callback block is retained |
| tock | ephemeral | background demand-driven generation; transaction-local materialization permitted |
| tock | persisted | background demand-driven generation; generated covered pages retained |

`ephemeral` does not forbid transaction-local materialization. `persisted` is a strict runtime
retention guarantee, not a best-effort cache or a promise of project-file
serialization. No persisted page is evicted for memory pressure, age, inactivity,
invalidation, or lack of readers. Retained pages are removed only when output
coverage ceases to include them; old storage versions are freed only after their
pins are released. A persisted Tick output becomes subject to that guarantee whenever
one callback leaves the output `written` or `voided`; the addressed callback block is
then mandatory recording work. An untouched callback block makes no retention change.
An author opting into persistence also opts into potentially unbounded memory use;
the compiler does not invent an eviction or recording policy to cap it. Backing
storage (RAM, file, mmap, etc.) is an implementation choice that must preserve this
guarantee.

`TickOutputConfig` does not imply that its output can only be read sequentially.
Persisted Tick data can be read at recorded published positions; an ephemeral Tick
output may also be randomly accessible when GraphJit proves contextual replayability.
No retention or access mode is inferred from a tiled expression: each member keeps
its own contract.

### Replayable node trait

A node opts into the intrinsic replay **type trait** by declaring
`static constexpr bool intrinsically_replayable = true` (default false).
`iv::details::intrinsically_replayable_v<Node>` is the validated compiler-facing
value; it is not a port field or an additional DSP callback. An opted-in node must define the
existing `tick()` but **not** its own native `tick_block()`, have no `State`, no
input/output history or latency after resolution, and zero internal latency. Random
Access inputs are permitted. Its tick computation must be deterministic and side-effect-free
under one immutable semantic/configuration version, declared inputs, and absolute
sample position; no mutable external or live-only resource may affect its result.
Validate instance-dependent properties such as internal latency after configuration.
Event replay, if enabled, must additionally preserve deterministic timestamp/order
and declared capacity; do not silently infer event replay from the sample rule.

Existing node traits already generate the `tick_block()` wrapper for `tick()`;
GraphJit already imports its LLVM definition. Replay uses **that** imported
wrapper in the background evaluation DAG, allowing normal O3 inlining, fusion,
value specialization, and possible SIMD. It does not redefine `tick()` for ordinary
nodes or introduce a second implementation of the DSP algorithm. GraphJit generates
identity-time forward/reverse propagation for mechanically pointwise dependencies.
When a replayable Tick node has a non-pointwise Random Access dependency, such as a
source addressed by a `GlobalIndex` coordinate stream, its background dependency
mapping is supplied through the same forward/reverse propagation contracts used by
background planning. Conservative whole-input/whole-output bounds are valid when no
tighter value-blind bound is available.

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

## 14. Persisted pages share the canonical whole-graph block quantum

All persisted outputs use one canonical persisted-page store abstraction regardless
of whether their producer is Tick or Tock. Pages are canonically aligned fixed-width
units whose width matches the fixed whole-graph root block size for the active layout
generation. This is a storage representation choice; semantic coverage remains
independent of sequential-input history and Tick-output history/latency.

For any persisted output page:

```text
page_interval(i) = [i * B, (i + 1) * B)
page_domain(i)   = page_interval(i) & output.coverage
```

A candidate page becomes valid only after its complete nonempty `page_domain` has
been materialized for the target `(semantic_version, page_version)` pair. One
`tock_coverage()` call may
cover many selected pages; page boundaries do not imply one callback per page.

Tick/persisted retention begins when a completed Tick invocation leaves its output
`written` or `voided`. Its addressed callback block enters the same persisted-page
store used by Tock/persisted output through the recording queue; no implicit recorder
or Tock callback is required. Random Access observes the data only through the
canonical published page view in the preliminary implementation.

Data representation may adapt to coverage occupancy: dense pages may store one
value per storage position, while coverage-packed pages store only covered
positions. The outer page directory should keep page-index order and small stable
handles while large data live in stable arena/chunk/file-backed storage.

### Changing the whole-graph block size repages stored data

Changing `B` changes storage partitioning, not DSP meaning. Persisted values are
losslessly repartitioned onto the new canonical page grid during a quiescent
layout transition. Sample values retain absolute indices; event data are
repartitioned by absolute index while preserving deterministic event order.

Tock/ephemeral outputs have no persisted data obligation requiring migration.
Tick-capture slabs are pre-publication/background-input storage, not a second
persisted representation. Recording and Tick/persisted staging may share the pool.
Consumed blocks become reclaimable after the transaction that consumes them commits,
subject to the callback-boundary rule for blocks visible to the audio thread. Old
immutable published snapshots may retain the old storage layout until their readers
release them.

## 15. Stored event pages, fan-in order, and random-access event iteration

Persisted event outputs use whole-page candidate validity with packed event
data.

A valid event page means the **complete event set for every position in the exact
covered page domain is known**, including the possibility of zero events.

`EventOutputProperties::max_events_per_index` remains density/capacity metadata,
not a literal per-timestamp quota. For one stored page:

```text
covered_positions    = measure(page_domain)
required_event_entries = ceil(max_events_per_index * covered_positions)
```

For a generated live event representation spanning `W` positions, the analogous
capacity uses the appropriate compiler-known effective fan-in bound over `W`.
When several event connections are combined into one buffer, GraphJit must account
for all incoming effective capacity bounds so every legal incoming sequence fits
without audio-thread allocation.

Background evaluation may dynamically commit persisted event data capacity off the
realtime thread. Tick-produced persisted events and explicit recording use
producer-specific reserve blocks supplied ahead of demand by the async
capacity manager; the realtime path consumes only already-assigned blocks and performs
no request-sized allocation. A future recent-data overlay could use recent queued event
payloads in front of published event pages, but the preliminary Random Access path
does not.

Event order is semantic and deterministic. Within one producer, events are emitted
in nondecreasing absolute sample-index order. Fan-in uses a stable tie break:

```text
1. absolute sample index
2. stable semantic source/connection index
3. producer-local event order
```

Page evaluation order, worker scheduling, cache/store layout, and request order
must never change equal-timestamp ordering.

Because one logical random-access event read can span multiple stored pages, node-facing
reads expose a segmented ordered range/iterator rather than promise one contiguous
`std::span<TimedEvent>`. Iteration preserves the ordering above and never exposes
events outside input coverage.

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

`TockState` is deliberately outside realtime `NodeStorage`. `BackgroundExecutor`
owns it as per-node background acceleration state, and it may contain dynamically
allocated structures. Reusable background workspaces are likewise background
sidecars rather than `NodeStorage` merely because they happen to be bounded.

`TockState` is not an authoritative persisted-output store and is not shared with
`tick_block()` or propagation callbacks. A node's observable behavior must remain
correct if its `TockState` is discarded or independently instantiated.

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
    // Versioned coverage/page/root state and stable data storage.
};
```

Ownership rules:

- every persisted output binds to one canonical persisted-page store entry keyed by
  stable output identity when that identity exists, regardless of Tick/Tock production;
- compatible executable generations rebind to that stable page-store entry rather
  than copying retained data;
- anonymous persisted outputs may use generation-local page-store identity;
- ephemeral outputs own no persisted-page entry to migrate or rebind; and
- provisioned blocks are runtime pre-publication/background inputs and are
  not a second published representation.

The canonical store has one immutable published snapshot root spanning all sample
and event entries selected by a transaction. Sample and event pages may use separate
typed payload implementations, but they share the same authoritative semantic/page
version and publication operation. There is no independent sample-page authority,
event-page authority, or generation-local persisted database. A candidate is mutable
only while private to its transaction; readers can acquire only an immutable published
snapshot.

Reader pinning must be suitable for the audio-thread boundary. Acquiring or releasing
a Tick reader pin must not allocate, block, take an unbounded lock, or synchronously
destroy the last owner of a retired snapshot on the audio thread. Publication and
reclamation therefore use a callback-boundary/epoch/RCU-style handoff, or an
equivalent design that defers destruction to a non-audio thread. A plain
reference-counted pointer whose final release can reclaim storage in `tick_block()`
does not satisfy this contract.

Persisted-storage reuse is normally **rebinding**, not copying. Page directories,
data backends, file/mmap-backed roots if ever used, and immutable snapshots may
remain owned by the executor while old/new executable generations refer to appropriate
versions. Disk backing is not implied by `persisted`; it would merely be one backend
of the same page-store abstraction.

The currently specified storage forms are therefore:

```text
current Tick representation
canonical persisted-page store for Tick/persisted and Tock/persisted outputs
transaction-local addressable materialization for background ephemeral Random Access
materialized sequential/addressable window for ephemeral Tick-time consumption
producer-specific reserve/pending-queue SPSC transports for persistence staging and explicit recording
```

These queues are block-backed and provisioned independently of background-evaluation
progress. Tick/persisted production uses them to move finalized Tick values toward the
canonical page store without requiring a Tock callback. When layout permits, a queue
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
  dependency on `BackgroundExecutor`, persisted pages or materialized storage;
- `PreparedCoveragePropagation` owns candidate coverage and exposes immutable exact
  changes/requirements plus the per-node activity selected by successful F/R,
  without mutating the committed baseline;
- `BackgroundEvaluationTransaction` owns one pinned base snapshot, the prepared
  propagation result, candidate pages, transaction-local materializations, invocation
  bindings and success/failure state;
- `BackgroundStorageRealization` interprets `BackgroundStoragePlan` indices for one
  selected range/version and produces explicit views, but does not decide demand,
  traverse nodes, invoke callbacks or publish; and
- the executor-level persisted-page store owns the only published persisted data and
  page-validity authority.

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

- the prepared activity/request selection used by the one-call-per-node Tock pass;
- selected stored-page completion plans;
- background-produced `tock/ephemeral` result/intermediate sample/event values;
- temporary event data and segmented views; and
- temporary references to immutable stored snapshots/pages.

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
commit; page arithmetic, materialization execution and callback binding stay in the
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
node's views; it is never given an executor pointer, the persisted-page store, a transaction
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

1. ordinary node configuration/resources and optional background `TockState` are
   initialized; `initialize()` may establish configuration/resource-dependent
   acceleration state, but not input-content-derived results;
2. current random-access-input coverages are available once upstream coverage is known;
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
to candidate persisted pages; only persisted-state/page publication extends the
Random-Access-visible retained snapshot.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Compatible stored state and
coverage are rebound unless semantic configuration, random-access input connection sets,
implementation semantics, sample rate, or another explicit forward cause requires
recomputation.

A `tock/persisted` candidate must become complete over its full new coverage
before publication. A `tock/ephemeral` output has no persisted precomputation obligation.

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published coverage or persisted-output semantics enters
through a `BackgroundExecutor`-controlled boundary. Other code must not mutate
active persisted roots/pages behind its versioning rules.

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
complete affected tock/persisted outputs
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
state/page version.

Queue publication itself is a complete background-work trigger. The worker pins the
available finite prefixes and derives the originating output, covered timeline ranges,
and changed ranges from the generation-local route plus each record header. Processing
those prefixes is not gated by an explicit evaluation call, an external coverage
request, or a previously successful background operation.

`BackgroundExecutor` owns the transaction environment used for that work. Page width is
the active compiled root block size. Semantic version is selected from the actor-owned
semantic state. Additional node mutations and external/advance demand are independently
accumulated actor inputs and may be coalesced with a queue-driven batch; they are not a
monolithic caller-supplied `BackgroundEvaluationRequest`.

External reads have a correspondingly closed entry-point set: application/UI
random-access fetches and advance background materialization for sequential playback.
The live `CompiledGraph::tick_block()` reads published/materialized data or supplies
its consuming sequential input's neutral value; it never runs tock.
Invalid `tock/persisted` page domains are internal materialization
roots, not a third external caller. The external protocol may combine updates and
fetches in one request; `BackgroundExecutor` normalizes that shape into mutation roots,
demand roots, and one or more legal background evaluation transactions.

## 21. Semantic versions, page versions, and stale-work rejection

Background computation may overlap Tick execution and may be superseded by newer
edits or by recording items that arrive after a running batch pinned its inputs. The
published page-backed output state uses two version coordinates:

- **semantic version** identifies graph/configuration/resource/sample-rate semantics;
- **page version** identifies one immutable published page view produced by
  a completed background evaluation transaction under those semantics.

A queued Tick recording item does **not** advance the page version. Items accumulate
as pending transaction input. A page version advances only when the background worker
has processed fixed producer-queue prefixes, propagated the transaction's invalidation roots,
completed any required Tock/replay work, filled the affected persisted candidate
pages, and atomically committed the successor snapshot. A semantic edit may likewise
create a new semantic version whose pages are built before publication.

Background evaluation transactions therefore operate against a coherent base/target pair:

```text
(semantic_version, page_version)
```

The reverse/evaluation phases of a background evaluation transaction use one
immutable base page version for existing random-access reads and produce one candidate successor. It never switches its base to
a newer publication in the middle of evaluation. An already-running transaction
may finish against an older immutable base, but stale-work/version validation must
prevent it from being committed as current when its semantic assumptions are no
longer valid.

For `tock/persisted`, a candidate is publishable only when every covered page
domain of every required computed output is valid for the target transaction.
Unchanged pages may be structurally shared from the immutable base. For
`tick/persisted`, the candidate structurally shares the prior snapshot and incorporates
all authoritative recorded callback blocks in the selected producer-queue prefixes;
the live Tick producer is not replayed merely to fill those pages.

For ephemeral Tock/replay results, no persisted-output validity exists; the selected
version chooses the configuration, coverage, sample rate, and immutable persisted
upstream pages against which the exact request is evaluated.

A queue-consuming pass owns the independently selected `(first,last)` prefix from
each relevant producer queue. The candidate may read exactly those selected blocks
plus already-published retained state. Blocks linked after a selected terminal block
are invisible to that pass and cannot change its coverage, invalidation roots,
reverse requirements, or Tock/replay inputs.

If the pass commits, publication promotes the prepared semantic/page state and the
selected queue prefixes may then be released according to their domain semantics. If
the pass is cancelled, fails during evaluation/materialization/publication, or is
rejected as stale, the committed semantic/page baseline does not advance and required
pending queue input remains available for retry rather than being silently consumed.

## 22. Direct connection compatibility and explicit recording

The port validator checks **each source channel** against the destination input's
access contract, not equality of the producer callback and consumer callback.
These are connection permissions, not guarantees of audio-thread scheduling:

| Source output | Sequential input | Random Access input |
| --- | --- | --- |
| Tick/ephemeral, unreproducible for demanded coverage | allowed | authored Tick persistence or an explicit recording node required |
| Tick/ephemeral, contextually replayable | allowed | allowed by background replay into an addressable materialization |
| Tick/persisted | allowed from the current Tick representation | allowed through the selected published persisted-page snapshot |
| Tock/ephemeral | allowed with a materialized sequential/addressable window | allowed with transaction-local materialization in background or materialized addressable data for Tick use |
| Tock/persisted | allowed from published pages generated ahead of playback | allowed through the selected published persisted-page snapshot |

An unreproducible Tick/ephemeral output cannot acquire implicit historical storage
merely because it feeds a random-access input; that is the **only special connection
prohibition** introduced by retention. The restriction applies whether the
random-access input is consumed by tick or tock. Tock outputs may feed sequential
inputs, and persisted tick outputs may feed random-access inputs. Tiling does not
change these permissions or silently materialize a new output: a whole-tile
random-access connection requires every selected source channel to satisfy it.
Projections preserve their source channel's original capability.

A Tock-produced output directly feeding another node's Random Access input must
be materialized into an immutable addressable representation before that consumer
evaluates. For an ephemeral output, a background-only consumer may use a
transaction-local page-backed materialization; a Tick-time consumer requires a
materialized addressable window that survives through the callback. Persisted pages
follow the strict retention guarantee. Replayable tick subgraphs
may be fused and materialized only where their consumers' addressing contracts
require it. Every tock callback, forward/reverse propagation callback, and page
recomputation runs off the audio thread.

An explicit recording node is required where an unreproducible ephemeral tick
stream must supply historical random-access demand. It declares an ordinary
Tick/persisted output; no additional recorder port kind or retention value exists.
Its semantics are fixed: output
writes overwrite the RAM recording at their timeline range, leaving an output
untouched preserves any previous recording there, and `write_void()` erases the
range authoritatively. Seeking changes the addressed timeline position, not those
rules. GraphJit does not insert an implicit generic recorder. Any independently
authored Tick/persisted producer is already recording its finalized output and needs
no additional recorder boundary.

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
    require canonical persisted pages
    require candidate/published page-version state and reader pins

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
a published persisted-page representation: their visibility semantics are different.

`RandomAccessInputConfig` by itself does not reveal whether node code reads that
input from Tick, Tock, or both. GraphJit should use statically known callback access
when available; until that is represented precisely, the safe planner rule is to
join every legal callback context for that input.

Derived converted/composed representations may be shared only when their source-atom
set, conversion/composition, temporal mapping, selected semantic/page version,
requested range, and lifetime/access contract match. This is what permits useful
fanout sharing without allowing one overlapping subset to promote or corrupt another.

### Preliminary Random Access visibility

The first implementation intentionally uses the simpler **published-snapshot-only**
Random Access rule. A Tick callback selects/pins its immutable persisted-page view at
the callback boundary. For a Tick/persisted producer, a block produced or captured
earlier in that same callback is therefore still invisible to Random Access even if
its eventual page position is already known. Pending candidate pages and sealed
capture records do not extend the selected view's coverage.

A later page-version publication makes those positions available to a subsequent
callback/read context. Publication occurring concurrently with a callback does not
mutate that callback's pinned view. Consequently, under this preliminary rule a
Tick/persisted-to-Random-Access connection adds **no same-Tick scheduling dependency**:
running the producer earlier in the callback would not change the snapshot the
consumer is allowed to observe.

This is deliberately more conservative than a possible future recent-data overlay.
Such an optimization could allow a downstream Tick Random Access consumer to read a
newly finalized Tick/persisted capture during the **same Tick**, but it would require:

- a same-Tick scheduling dependency from the producer to that consumer;
- an immutable recent-capture lookup layer in front of published pages;
- a branch/split when a requested range crosses from published pages into recent
  captures (and an ordered two-source merge for events);
- one semantic/version lineage so newly published pages supersede, rather than
  duplicate, the recent overlay; and
- callback-boundary-safe block reclamation.

The preliminary implementation does not pay that complexity.

Tick-visible ephemeral advance materialization is not a published persisted-page
version. The runtime object that pins those immutable views for one callback should be
named `TickMaterializationSnapshot` (or an equivalently explicit local name), not a
"published materialization." It has one owner/selection path, is selected at the
root boundary, and remains alive through all callback readers, but it does not acquire
persisted-output retention or page-version semantics.

### Sequential playback and page availability

A sequential input consumes its selected pinned published page **as-is**, even if
that page is stale or invalidated for a pending candidate. A genuinely missing page
produces that **input's own `neutral_value`** (including when members of a tile
have different neutral values). Stale does not mean missing: an invalidated old
page remains readable until a replacement is atomically published. The audio thread
never waits for a background evaluation transaction, invokes `tock_coverage()`, or allocates
a page on demand. A coherent pinned snapshot prevents concurrent publication from
mutating memory under an audio callback. The neutral fallback does not authorize
out-of-coverage arbitrary random-access reads by node code; those remain invalid.

### Provisioned queue items and Tick/persisted recording boundaries

Realtime-produced data whose lifetime must escape ordinary current-block execution
uses producer-specific producer reserves and background pending queues. An explicit
recorder is simply an authored node whose Tick/persisted output stages finalized data
for canonical page publication; it may consume otherwise unreproducible Sequential
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

For `tick_block()`, the disposition applies to the entire addressed callback block:
`written` publishes one authoritative overwrite for that block and `voided` publishes
one authoritative erase for that block. Scalar `tick()` has the same rule with a
one-sample block. History and latency remain legal mutation/storage facts, but do not
extend a recording item to `[block-history, block-end+latency)`. Recording start and
stop precision therefore follows Tick callback boundaries.

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

## 23. Complete persisted outputs and backing storage

The persisted-page store is canonical for both Tick/persisted and Tock/persisted
outputs. A candidate successor is never exposed as a partially complete published
snapshot. Tock/persisted completion may require its entire exact candidate coverage
to be materialized before publication; Tick/persisted pages become eligible when
their recorded callback blocks are incorporated into a coherent successor page
version.

Complete materialization does not require all bytes to remain in anonymous RAM.
Backing storage may use mmap/files, compression, deduplicated immutable pages,
or copy-on-write roots, but **never automatic eviction** of persisted covered
data. Invalidation retains readable old published pages while a successor is
computed. Pages may be removed from the logical retained output only when its
coverage no longer includes them. Superseded immutable storage versions may be
reclaimed after their readers release them. Any representation accessed directly by the audio-thread Tick path must meet
audio-thread access requirements.

Tick/persisted output differs only in **how persisted pages are populated**:
`tick_block()` may revise storage under normal history/latency semantics, while each
recorded Tick invocation authors its addressed callback block as a whole or leaves it
untouched. Tock output retains exact arbitrary-coverage authoring and does not acquire
this block disposition. Once published, Tick- and Tock-produced persisted data use the
same page lookup, versioning, pinning, and consumer read path.

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
A successful transaction commits the semantic/page result corresponding to its selected
work. Only after successful domain commit may the selected prefixes be released. A
failed/cancelled/stale transaction does not silently consume input required for retry.

Released blocks from all producer queues may enter one SPSC released-block stream from
the single background worker to `AsyncCapacityManager`. Each block carries enough
owner/type information for reclamation/reassignment. Background never directly splices
completed blocks into a realtime reserve.

When background produces a coherent immutable persisted-state/page version, it
publishes it through a latest-version mailbox to realtime. Intermediate pending
versions may be superseded before realtime reaches a pass boundary; superseded objects
are retired off realtime. Realtime activates only the newest compatible pending pointer
at a legal boundary.

If a persisted-page implementation adopts queue payload storage, ownership must still
be explicit: adopted storage belongs to that immutable page/version until it retires,
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
and Tock/persisted outputs use the same published persisted-page read path. At the
callback boundary, realtime adopts at most the newest generation-compatible mailbox
root, which already pins one exact persisted-page/Tick-materialization pair. Current
Tick buffers, pending candidates, and newly queued realtime blocks are not alternate
lookup sources.

Ephemeral Tock/replay results use immutable addressable materializations instead of
persisted pages. Background-only consumers may use transaction-local materialization.
A Random Access input used during Tick execution requires the needed ephemeral result
to have been materialized into an immutable addressable window before the callback.

Random-access event reads may span several stored segments/pages and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Random-access accessors expose exact input coverage. Debug validation catches
requests outside coverage. A `tock_coverage()` accessor reads Random Access inputs from the selected immutable
transaction view, which may include canonical persisted pages regardless of Tick/Tock
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
`(semantic_version, page_version)` pair from which it was read.

For `tock/persisted`, a newer candidate is not externally presented as a partially
materialized stored output. Until the entire required stored candidate is complete,
the newer version is pending/not-ready and callers may continue using an older
completed published result. Missing candidate pages must never be represented as
neutral samples, zero events, or artificial missing coverage.

For `tock/ephemeral`, an external request may evaluate exact requested coverage
against a selected immutable version pair and return that pair with the result. The
host API may satisfy the request synchronously or expose explicit pending/future
behavior; the exact ABI is implementation work.

For Tick/persisted output, sealed captures newer than the selected published page
version are likewise pending, not an alternate external read source. The old
published snapshot remains coherent until a successor incorporating the fixed capture
prefix commits. This is the deliberate preliminary "out-of-date read" behavior.

A caller must not combine independently returned data from different semantic or
page versions as if it were one coherent random-access result.

## 27. Outward change notification

After a candidate page version becomes published—including a transaction that
consumed recorder or Tick/persisted capture records—the executor may emit a coalesced
output change notification for subscribed external consumers.

Conceptually it contains:

```text
output identity
semantic version/revision
published page version
new coverage (or a coverage-changed indication)
changed Coverage (sound may-change superset)
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
  invalid candidate pages before publication;
- tick/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no persisted data to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic port identity rather than builder
handles, lowered node indices, or ORC-generation-local indices.

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
page-store or transaction pointer.

The generated Tick root remains a zero-project-port root but gains an explicit
invocation frame for dynamic background-derived bindings. GraphJit retains the
compile-time mapping from node ports to compact frame slots, and `RealtimeExecutor`
selects one immutable published-page snapshot plus any `TickMaterializationSnapshot`
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

- stable canonical persisted-page stores/immutable roots for **all persisted
  outputs** (`tick/persisted` and `tock/persisted`), plus per-generation bindings;
- semantic versions plus immutable page versions and candidate/published snapshots;
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

Implementation choices include page backing, snapshot-directory representation,
the concrete callback-boundary/epoch mechanism used for audio-safe reader pins,
transaction-workspace reuse, queue block sizes and capacity-watermark strategy,
worker scheduling, cancellation granularity, and post-correctness SIMD/fusion/
value-specialization cost models. None of these may weaken the authored persisted
retention obligation or introduce audio-thread tock execution.

The following are **not** implementation choices: static constexpr concrete node
port schemas; independent input access/output production/output retention; explicit
opt-in replayability with contextual dependency validation; no implicit recording of
unreproducible Tick/ephemeral sources; one canonical persisted-page read abstraction
for Tick/persisted and Tock/persisted outputs; background-only Tock and propagation;
published/materialized-snapshot-only Tick-time Random Access in the preliminary
implementation; stale-page-as-is/missing-page-per-input-neutral Sequential playback;
subset-first storage requirement inference before storage coalescing; no persisted
page eviction except loss of covered positions; exact output coverage and sound
conservative forward invalidation; value-blind conservative reverse demand; fixed
capture-prefix transactions; callback-boundary-safe
capture-block reuse; transaction-wide coverage/page/frontier commit; compiler-owned
materialization scheduling; narrow transaction-local generated-root hooks with no
executor/runtime-owner backdoor; legal-size replay invocation splitting; and no
unresolved Random Access evaluation cycles.

## 31. Static validation

Node/package validation rejects non-static or non-constexpr concrete port schemas
at **every** construction path (including internal builder paths), malformed
`TockState`, conflicting callback/production declarations, invalid event
capacity declarations, and invalid replayability trait declarations. The planned batch
API additionally rejects ambiguous authored shapes: Tick authors exactly one of
`tick()`/`tick_block()`/`tick_block_batch()`; a node with Tock outputs authors
exactly one of `tock_coverage()`/`tock_coverage_batch()`; and skip plus each authored
propagation direction choose at most one scalar-or-batch form as specified in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

The checked-in replayability validation still requires `tick()` without native
`tick_block()`, no `State`, no random-access inputs, zero port history/latency and zero
internal latency. This is an implementation checkpoint, not the target semantic rule:
the target replayability trait removes the prohibition on Random Access inputs while
retaining the deterministic/side-effect-free, state/history/latency restrictions. The
later batching/replay direction moves the callback-shape test to the normalized
`do_tick_block()` semantics so eligible authored `tick_block()` and
`tick_block_batch()` implementations can participate without source-shape special
cases; the replay contract still requires pure deterministic evaluation under a fixed
version.

Whole-project GraphJit validation resolves per-channel connections and semantic
SCCs, derives contextual replayability and exact available coverage, rejects
unreproducible tick/ephemeral -> random-access demand unless an explicit recorder
intervenes, and rejects unresolved background evaluation cycles. A Tock ->
Random Access consumer requires an immutable addressable materialization; persisted
outputs use canonical pages, while ephemeral storage lifetime depends on the
consumer callback context. Explicit recorder bridges must have their valid authored
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
stale-base rejection, old-or-new whole-snapshot reader visibility, complete page-domain
validation, stable and anonymous output identities, valid zero-event pages,
deterministic equal-time event ordering, missing Sequential-input neutral values, and
the prohibition on audio-thread Tock/allocation/blocking/final-owner reclamation.

## 32. Implementation landing order

This is the **normative dependency order** for the next documentation/implementation
migration; [graph_jit_direction.md](./graph_jit_direction.md) and the application
architecture should reference this list rather than propose a divergent one.
The legacy executor deletion, final schema/intrinsic trait changes, and the
connection/background-dependency planner rewrite are now separate landed stages
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
   canonical persisted-page bindings, current-Tick views, materialized windows,
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
   with page publication when a candidate is present.

   Complete this checkpoint in the following dependency order:

   1. **Landed:** extract the reusable propagation machinery as a standalone runtime
      component, separate committed coverage state from it, introduce
      prepare/commit/discard semantics, and remove the interim propagation-only
      executor API;
   2. **Landed:** retain immutable per-node materialization placement, runtime
      binding-slot maps plus replay invocation constraints/schedule slots in the
      compiled plan, with validation;
   3. **Landed:** add the executor-level canonical sample/event persisted-page store
      with private candidates, one immutable published snapshot root, stale-base
      checks and audio-safe reader pins. `PersistedPageStore` owns a single atomic
      root containing both typed sample and event page handles. Candidates
      structurally share unchanged immutable pages, remain private until whole-root
      publication, and carry both semantic/page coordinates; a second candidate
      from a superseded root is rejected without changing the publication. Stable
      output identities and explicit generation-local identities use the same store.
      Reader slots are registered off the audio thread; pin acquire/release performs
      only bounded atomic operations and holds a non-owning root pointer. Superseded
      owning roots enter a retired list and are destroyed only by an explicit
      non-audio reclamation pass after no slot pins them. Existing retained pages
      currently reject a page-width change until the later explicit repaging/
      generation-transition stage is implemented;
   4. **Landed:** add address-stable runtime views and small sample/event
      direct, conversion, projection, fan-in and merge operations, then realize
      the complete `BackgroundStoragePlan` for transaction-local and persisted
      storage. `BackgroundStorageRealization`
      resolves plan storage indices into stable opaque sample/event read/write views,
      owns sparse transaction-local and future materialized-window buffers, binds
      non-owning published-page readers under the surrounding transaction's pin,
      derives stable or generation-local persisted identities outside the page store,
      and seals only after required views are compatible. It now also executes each
      compiler-retained runtime operation at most once against those sealed views.
      Direct sample/event operations remain copy-free alias validation; sample
      materialization applies per-source read latency plus the planned heterogeneous
      conversion/projection assembly; event materialization applies the immutable
      non-expanding conversion and reuses the shared stable k-way merge, preserving
      `(absolute time, semantic source order)`. It still does not traverse nodes,
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
      narrow post-forward demand policy complete invalid or missing persisted pages
      before reverse propagation, derives logical-binding and physical-storage
      selections, and owns a private page candidate only when persisted page state
      actually changes. The call-frame owner allocates
      isolated power-of-two replay sample/event storage, populates inputs after the
      compiler-placed prepare operations, flushes outputs before the placed finalize
      operations, and binds no live realtime state. A replay node with several outputs
      flushes each output only over that output binding's selected coverage, even
      though the imported Tick wrapper runs over the union schedule. The transaction
      first seals every pinned persisted input only after proving that its selected
      coverage is present in compatible published pages. It then invokes the generated
      evaluate root once with the prepared activity and
      verifies that every selected produced sample/channel was initialized and that
      no sample/event callback write was rejected before staging any page. Failure,
      including incomplete sample production, discards all
      transaction-local values; stale-base rejection promotes neither pages nor
      semantic coverage. A transaction that produces no persisted page data and no
      invalidation does not advance the page version, but still preserves the pinned
      base's semantic-version monotonicity and nonempty page-layout compatibility and
      revalidates that base as current before promoting semantic coverage. When page
      state does change, successful publication precedes the propagation workspace's
      non-throwing coverage promotion. `BackgroundExecutor`
      exposes this end-to-end operation and no propagation-only compatibility API; and
   6. **Complete:** Tick-time dynamic bindings use
      `TickMaterializationSnapshot` playback and per-input neutral values for genuinely
      missing sequential data. The generated Tick root's final ABI accepts one
      `TickInvocationCall` containing only resolved sequential and Random Access view
      spans. `RealtimeExecutor` registers both reader slots on the control path and constructs
      a non-copyable callback-scoped `TickInvocationFrame`; that owner acquires one
      bounded atomic page-root pin and retains it across the complete generated-root
      invocation. Generated code receives neither that owner nor a page-store/executor
      pointer. Immutable compiler slot maps now assign each node's dynamic Sequential
      and Random Access sample/event inputs contiguous kind-specific ranges in the
      invocation record. Sequential slots retain persisted-page and
      Tick-materialization candidates; generated-root current-Tick composites remain
      operations rather than external binding slots. Sample neutral values remain
      properties of their logical input ports.
      A realization-owned,
      address-stable `TickInvocationWorkspace` resolves persisted identities on the
      control path. Random Access callback binding only retargets its preallocated
      views to the pinned root, while Sequential sample binding refreshes its bounded
      preallocated rings. Published snapshots precompute exact per-output coverage metadata,
      so direct identity sample/event page views require no callback-time allocation or
      coverage construction; generated lowering passes the corresponding node-local
      subspans to imported Tick wrappers. Successful background transactions now also
      freeze compiler-routed derived, converted, fan-in and ephemeral Random Access
      results, plus direct background-only Sequential materializations, into one
      immutable `TickMaterializationSnapshot`. Mixed `current_tick` composites remain
      generated-root work because they also consume live Tick sources. Its promotion is a
      no-fail owner relink after any page publication and before semantic coverage
      promotion. Background then publishes a mailbox root that pins that materialization
      together with its exact page root. Realtime adopts the pair at a pass boundary
      without reference counting; retired pair owners are reclaimed only by an explicit
      non-audio executor operation. Each snapshot records its compiled generation and
      exact persisted-page version, and an incoherent pair is rejected before mailbox
      publication rather than repaired inside a callback. Sequential sample slots own
      preallocated power-of-two rings
      sized from the specialization's maximum block size plus authored history. The
      callback fills only the bounded requested window from coherent materialization
      or direct page data and leaves absent frames at that logical input's neutral
      value, without allocation. Sequential event slots likewise own preallocated
      power-of-two sequences sized from the selected storage's aggregate event rate
      and maximum block size. Callback binding resets the count and copies only events
      in the requested absolute-time window; missing data is the empty sequence.
      Generated lowering now overlays each node's compact
      Sequential slots onto a stack copy of its complete ordinary input-binding array,
      preserving unaffected and mixed live inputs. Sequential sample and event spans
      are both supplied by callback binding.

   Final commit atomically promotes prepared semantic coverage plus any candidate page
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
   retained state/page version, and release consumed blocks. One background worker may
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
   omission, or coverage erasure in the canonical retained pages.

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
   and authored-future latency are distinct roles. Every entry retains its exact extent
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
   stream is truthfully marked non-recoverable instead of being mistaken for one
   producer's state. Materialization names a storage operation rather than necessarily a
   type conversion: a same-type retained fan-in may have a zero-step conversion plan while
   still materializing the selected invocation window for its consumer.

   The next migration substep resolves the remaining truthful non-recoverable
   realizations: multi-source sample compositions, callback-only event streams, and event
   fan-in that erased producer identity. It must trace derivations where possible and
   otherwise plan private transition retention. Only then can old/new stable port-state
   identities be compared and their overlapping temporal ranges assigned direct migration
   or a finite transition realization.

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
   cross-node SIMD, fusion, page placement, transient reuse, storage aliasing/liveness
   and immutable-value specialization only after graph-version state continuity is
   covered by tests.

## 33. Summary invariants

1. Concrete GraphJit nodes have static constexpr port schemas; only graph topology
   and instance/connection composition are dynamic.
2. Input access (`SequentialInputConfig`/`RandomAccessInputConfig`), output
   production (`TickOutputConfig`/`TockOutputConfig`) and output retention
   (`ephemeral`/`persisted`) are independent declarations.
3. `tick()` already lowers to an imported, optimizable generated `tick_block()`.
   The planned batch layer retains `tick()` as a first-class authoring form and
   normalizes every accepted Tick shape to both `do_tick_block()` and
   `do_tick_block_batch()`. A separately declared node trait establishes intrinsic
   replay eligibility; GraphJit proves contextual replayability through available
   upstream coverage.
4. An unreproducible Tick/ephemeral source cannot directly satisfy Random Access
   demand. Tick/persisted, replayable Tick, and either Tock output can. Persisted
   outputs use canonical pages; ephemeral Tock/replay results use immutable
   addressable materialization with transaction or materialized-window lifetime according
   to the consumer callback context.
5. Tiling preserves per-channel contracts and creates neither an implicit recording
   policy nor an implicit producer.
6. `tock_coverage()`/`tock_coverage_batch()` and authored scalar/batched propagation
   callbacks run only off the audio thread. A sequential input plays a stale published
   page as-is and substitutes
   **its own** `neutral_value` for a missing page without waiting.
7. Persisted outputs retain all generated/finalized covered data without automatic
   eviction. Coverage removal alone ends the semantic retention obligation;
   superseded storage versions are reclaimed only after readers release them.
   Memory growth is author-selected.
8. Exact output `Coverage`, conservative forward may-change regions and conservative
   reverse requirements are independent of the storage page grid. Both dependency
   directions may over-approximate but never under-approximate actual dependencies.
   Mechanically pointwise replayable Tick propagation is generated from static
   dependencies; non-pointwise Random Access footprints use authored background
   propagation.
9. The background evaluation DAG has no unresolved cycles; feedback in the
   same-Tick scheduling graph retains its separate temporal semantics.
10. Realtime-produced recording and Tick/persisted data use producer-specific reserves and background pending queues backed by power-of-two blocks. Producers build chains
    privately and publish them cheaply at pass boundaries; background independently
    pins one finite prefix per queue. There is no cross-queue atomic snapshot. Queue
    insertion is not persisted-state/page publication, and released blocks return
    through non-realtime capacity management.
11. `TockState` is non-semantic Tock-only acceleration; canonical persisted-page
    stores and pending producer queues are background-owned sidecars, distinct from
    realtime fixed `NodeStorage`.
12. Storage requirements are joined over exact overlapping port atoms, not chosen
    independently per connection or promoted wholesale per authored port. storage
    coalescing happens only after those correctness requirements are known.
13. The legacy generated-node graph executor and dynamic concrete-port fallbacks
    are deleted; the package/configuration JIT and GraphJit's imported concrete-
    node LLVM remain.
14. F/R propagation produces prepared coverage only. Coverage/page publication and
    release of the selected producer-queue prefixes become authoritative together at
    final transaction commit; every failure path leaves committed state and required
    pending inputs unchanged.
15. The generated background root owns static node/materialization interleaving and
    calls only narrow transaction-local prepare/finalize hooks. Generated Tick and
    background roots receive explicit binding frames, never executor pointers, a page
    store, queue/capacity-manager internals, or a transaction object.
16. Tick reader pins and `TickMaterializationSnapshot` lifetimes cross one complete
    root callback without audio-thread allocation, blocking or final-owner
    reclamation. Replay work is split into compiler-legal block-sized invocations.

# Coverage, Random Access, And Background Evaluation

[DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md) defines the normative vocabulary used here.

This document is the normative design for **coverage and random-access DSP semantics**,
their incremental background-evaluation model, and the executor-side
storage/publication rules needed to make data at global positions usable by both
random-access consumers and Sequential inputs during Tick execution.

Older revisions used `indexed` as an umbrella term for several independent
properties that are now named directly: random-access consumption, Tock
production, output retention, coverage, background evaluation, and persisted-page
storage. Existing C++ identifiers such as `IndexedCoverage`, `IndexedState`, and
`IndexedPlan` retain their source spelling until a separate source/API rename;
that spelling does not define an architectural category.

The intended callback vocabulary is:

```cpp
tick_block(...);                       // Tick execution over Sequential inputs
tock_coverage(...);                    // one-node background evaluation over coverage
propagate_forward_coverage(...);       // one-node changed inputs/state -> outputs/coverage
propagate_reverse_coverage(...);       // one-node required outputs -> required inputs
```

The `_coverage` callbacks each operate on **one node** even when the supplied
`IndexedCoverage` contains many disjoint regions. This deliberately differs from
existing names such as `tick_block_batch`, where `batch` means that multiple
nodes are processed as one compiler/runtime operation. A future multi-node
background-evaluation interface may therefore use the names:

```cpp
tock_coverage_batch(...);
propagate_forward_coverage_batch(...);
propagate_reverse_coverage_batch(...);
```

Those future batch callbacks would process a batch of nodes, each with its own
coverage/state. They are not the semantics of the current one-node callbacks.

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
> or place a recording node with an explicit retention policy. The restriction
> prevents **implicit recording**, not a technically impossible connection. The
> preliminary Tick-time Random Access path reads only a pinned published/prepared
> immutable view; it never aliases current mutable Tick output.

> Tiling is a non-materializing composition of source channels. It preserves each
> source's production, retention, and effective access capabilities. A whole-tile
> random-access connection requires every selected source channel to satisfy the
> requested random-access contract; channel projection retains its own capabilities.

> Computed tock outputs publish exact finite `IndexedCoverage`. Forward
> propagation records exact semantic coverage/change without forcing evaluation;
> reverse propagation is value-blind and may conservatively over-request. Page
> boundaries never widen semantic coverage or forward changed regions. A replayable
> pointwise tick node's temporal propagation may be synthesized by GraphJit.

> Invalidation and requested coverage are processed as batched background
> evaluation transactions: all roots accumulate
> before traversal; fan-in/fan-out requirements are unioned; each applicable authored
> forward/reverse/tock callback runs at most once per implicated node per transaction.
> `IndexedState` remains non-semantic acceleration available only to tock.

> No unresolved random-access computation dependency may participate in a cycle.
> Replayable tick subgraphs must be acyclic when expanded for background demand;
> valid published persisted values can terminate replay traversal. Ordinary
> Tick-to-Sequential feedback retains its existing scheduling semantics.

> Persisted output pages are **never evicted** for age, cache limits, memory
> pressure, or invalidation. Retained generated pages may be removed only when
> output coverage no longer includes them; replacement physical versions are
> reclaimed only after their readers release them. Unbounded memory use is an
> explicit consequence of the author's persistence declaration.

> An explicit recording node captures otherwise unreproducible sequential data at
> its production point into already-provisioned slab-backed storage. Each background
> pass snapshots a fixed capture-sequence prefix; captures enter ordinary exact
> invalidation, reverse planning, background computation, and one atomic page-version
> publication. The capture log is temporary transaction input, not persisted pages.

This is an incremental coverage-evaluation model integrated with Tick execution,
not a second audio-thread scheduler and not a storage class.

## 1. Port model

Port payload (sample/event), input access, output production, and output retention
are independent. The callback that *uses* an input does not determine that input's
access: a `tick_block()` implementation can consume random-access inputs, and a
replayable `tick()` node has only sequential inputs even during background replay.

| Axis | Authored alternatives | Meaning |
| --- | --- | --- |
| input access | `SequentialInputConfig`, `RandomAccessInputConfig` | bounded sequential window or arbitrary positions inside coverage |
| output production | `TickOutputConfig`, `TockOutputConfig` | existing generated tick-block implementation or background tock callback |
| output retention | `OutputRetention::{ephemeral,persisted}` | whether generated finalized values must be retained |
| payload | sample, event | value/event representation and bounds |

Sequential input history and tick output history/latency are finite timing
contracts. Random-access input and tock output declarations do not acquire these
fields. Output retention never selects a callback. The concrete sample/event
schema is in section 13; the direct-connection rule is in section 22.

`tick()` already exists: the node traits generate its `tick_block()` wrapper, and
GraphJit imports that wrapper's LLVM definition. The new replayability trait
permits **some** such nodes to execute through that same wrapper in the background
evaluation DAG. It does not make all `tick()` implementations pure or replayable.
No `tock_coverage()` call occurs on the audio thread.

## 2. `IndexedCoverage` is the only coverage boundary

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
replayable `tick()` subset itself has no random-access inputs. An ordinary
unretained, unreproducible tick stream has no arbitrary historical coverage.

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

## 3. `IndexedCoverage` helper

Node and runtime code should use one small helper value type rather than manually
maintaining vectors of regions.

Conceptually:

```cpp
struct IndexedRegion {
    SampleIndex begin;
    SampleIndex end; // half-open [begin, end)
};

class IndexedCoverage {
public:
    using Region = IndexedRegion;

    IndexedCoverage() = default;
    explicit IndexedCoverage(Region);
    explicit IndexedCoverage(std::span<Region const>);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(SampleIndex) const noexcept;
    [[nodiscard]] bool contains(Region) const noexcept;
    [[nodiscard]] bool intersects(Region) const noexcept;

    void include(Region);
    void include(IndexedCoverage const&);
    void exclude(Region);
    void exclude(IndexedCoverage const&);

    [[nodiscard]] IndexedCoverage intersection(IndexedCoverage const&) const;
    [[nodiscard]] IndexedCoverage difference(IndexedCoverage const&) const;

    auto begin() const noexcept; // region iterator
    auto end() const noexcept;   // region iterator
};

IndexedCoverage operator|(IndexedCoverage const&, IndexedCoverage const&);
IndexedCoverage operator&(IndexedCoverage const&, IndexedCoverage const&);
IndexedCoverage operator-(IndexedCoverage const&, IndexedCoverage const&);
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
IndexedCoverage shifted(IndexedCoverage const&, SampleIndex offset);
IndexedCoverage expanded(
    IndexedCoverage const&, SampleIndex before, SampleIndex after);
```

The same type represents output coverage, exact semantic change regions, and
exact reverse requirements. Cache validity is deliberately coarser and is not
represented by one `IndexedCoverage` bit per sample.

## 4. Coverage, stored page domains, validity, and payload

The executor distinguishes several concepts for persisted outputs:

- **coverage**: exact regions where the output exists and may legally be requested;
- **page domain**: for one physical stored page, `page_interval & coverage`;
- **validity**: whether that whole page domain is current for a candidate
  `(semantic_version, page_version)` pair;
- **payload**: retained sample/event values for the page domain;
- **semantic version**: the dependency/configuration environment for which those
  values are meaningful; and
- **page version**: the immutable published page view against which those values
  were computed or read.

Coverage is exact and independent of physical page boundaries. Stored pages for
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

Physical RAM residency is intentionally not part of coverage or retention semantics. A complete
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
If an exact semantic change intersects one or more stored pages, those page domains
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
demand. Semantic changes to any published retained source seed downstream exact
invalidation; they do not erase old readable pages.

Thus stored-page width is **not** an independent persisted-page storage tuning knob. It is
the active whole-graph root block size. Choosing a smaller or larger root block
therefore changes both Tick scheduling granularity and stored-page granularity,
while coverage and external-request semantics remain unchanged.

## 6. Exact propagation remains independent of page granularity

Page granularity is a stored-materialization decision only. Semantic dependency
propagation remains exact in `IndexedCoverage` region space.

If a mutation semantically changes:

```text
[523,530)
```

forward propagation sends exactly `[523,530)` downstream. If that region
intersects a `tock/persisted` candidate page, the whole page becomes invalid locally,
but the executor does **not** widen the changed region to the whole page before
propagating it farther.

When the executor later rebuilds that stored candidate, it selects the complete
covered page domain before reverse propagation through the producer because the
producer is committed to materializing that entire page domain:

```text
exact semantic change
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
reverse-propagated directly.

This is the deliberate asymmetry:

- forward semantic change is never widened merely because a stored page becomes
  invalid;
- candidate completion for `tock/persisted` may page-promote work because the stored
  version must become complete; and
- demand through `tock/ephemeral` remains exact because no retained output page is
  being completed.

## 7. Forward changes are independent of demand

Forward propagation is independent of random-access consumption and is not limited
to changes arriving through random-access input connections.

The executor has a closed set of **external invalidation roots**. Every external
semantic invalidation root for background coverage propagation belongs to one of
these classes:

1. **node-local semantic mutation**: an application/UI operation changes node
   state, configuration, or a resource according to that node type's own semantic
   rules;
2. **Tick-capture snapshot**: a fixed prefix of newly sealed capture blocks changes
   one or more capture-backed outputs at their recorded global positions. This covers
   explicit recorder outputs and Tick/persisted staging; capture insertion itself is
   not persisted-page publication;
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

1. the output's new exact `IndexedCoverage`; and
2. exact regions inside the old/new coverage whose values may have changed.

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
candidate. The exact `changed` set continues downstream unchanged by page
boundaries.

The forward phase does not recursively call `tock_coverage()`. It produces the
batch's exact semantic changes and invalid stored-page set. The same logical batch
may then use those invalid page domains as materialization roots for a later
reverse/evaluation phase before publication; the phases remain distinct even when
the executor runs them back-to-back.

## 8. `propagate_forward_coverage()`

The forward dependency callback answers:

> Given this node's current random-access-input coverages, exact changed random-access-input
> regions, current semantic configuration, sample rate, and any local semantic
> change cause, what are the resulting computed-output coverages and which exact
> output regions may have changed?

Conceptually:

```text
F(node): input coverage + changed input regions + semantic configuration + sample rate
      -> output coverage + affected output regions
```

It runs in forward graph direction. Incoming changed regions from all random-access
inputs and all invalidation roots belonging to the batch are accumulated/unioned
before the node is visited. **Each implicated node is visited at most once by the
forward phase of one background evaluation transaction.** A node may still have many disjoint changed
regions and multiple changed inputs in that one callback invocation. Replayable
tick pointwise nodes use compiler-synthesized coverage propagation in the same
forward/reverse orders.

For every node that declares a computed tock output, an explicit forward-coverage
implementation (or a future equivalent multi-node/batched implementation) is
**mandatory**. Coverage is semantic and exact;
there is no generally correct fallback that can invent it. A framework may still
synthesize trivial glue when exact coverage is mechanically declared by another
static facility, but it must not silently preserve old/empty coverage or substitute
a conservative superset.

A tick-only replayable pointwise node needs no authored forward or reverse
coverage callback: GraphJit synthesizes the exact same-position temporal mapping
from its static input/output dependencies. A tick-only node with persisted output
provides a stored invalidation boundary for finalized data; it does not acquire a
`tock_coverage()` implementation. A recording node's tock-produced output remains
an ordinary computed output with exact forward-coverage semantics and background
`tock_coverage()` materialization.

`IndexedState` is **not** visible here. Forward dependency semantics must not vary
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
  propagation merely because an older physical page still exists. Its complete
  covered page domain becomes a materialization requirement and reverse planning
  continues through its producer; and
- `tock/ephemeral` owns no persisted result and therefore never forms a retained
  reverse cut.

Thus page retention and page semantic validity are deliberately distinct. Never
dropping old immutable page payloads does not make those payloads current for a
new semantic version.

After the callback reports input requirements, every requirement is clipped to the
input's exact coverage.

The callback is **value-blind**. It may inspect coverage, semantic node
configuration, sample rate, and other deterministic structural metadata, but it
must not inspect random-access input payload values to discover a second-stage
dependency footprint. If exact addressing depends on a random-access control input,
the callback returns a conservative superset derivable without reading that
signal, up to the complete potentially relevant input coverage. A future staged
value-dependent dependency facility may relax this rule without changing the
initial ABI.

The callback may be omitted where the conservative exact rule is mechanically
known, for example by requiring all covered regions of every random-access input.
Output-only Tock sources have nothing upstream to request and therefore require
no reverse callback.

For a tock-produced output needed by sequential playback, reverse planning and
all required computation run off the audio thread. The audio thread only reads
the already published/prepared result; an unavailable page uses the sequential
consumer's `neutral_value`.

`IndexedState` is **not** visible here. Dependency requirements may not depend on
memoization history.

## 10. `tock_coverage()`

`tock_coverage()` is the one-node background-evaluation callback for outputs whose
output production is `TockOutputConfig`. Its context carries requested
`IndexedCoverage` per Tock output; one invocation may therefore
compute many disjoint regions and several outputs of the same node.

Within one background evaluation transaction, reverse planning first finishes accumulating the final
requested coverage for every implicated output. Forward evaluation then calls
`tock_coverage()` **at most once per implicated node for the whole batch**. Requests
for several outputs, several disjoint regions, and several downstream consumers
are therefore coalesced before the callback runs.

The callback receives only work that needs computation for the target operation:

- a `tock/ephemeral` output receives exact demanded covered regions; and
- a `tock/persisted` output receives selected invalid candidate page domains, each
  already intersected with exact output coverage.

It never receives uncovered portions of a physical page.

The callback:

- sees random-access inputs and computed tock outputs;
- sees the project sample rate for the semantic version being evaluated;
- operates on covered global sample/event regions;
- cannot request random-access inputs outside their coverage;
- cannot depend on mutable or unavailable live tick values; finalized persisted
  tick outputs and contextually replayed tick values are valid sources through
  random-access inputs;
- cannot depend on sequential `State`;
- may use `IndexedState` solely as non-semantic acceleration/memoization state; and
- must produce observable results independent of background request order and the
  contents/history of `IndexedState`.

`IndexedState` has one deliberately narrow meaning:

> It may change the pace or implementation strategy of `tock_coverage()`, but it
> may not change output values, output coverage, reverse requirements, or any other
> observable semantics.

A correct node must therefore produce the same result from a freshly initialized
`IndexedState`. The executor is free to serialize access, duplicate state per
worker, discard/reinitialize it, or otherwise manage acceleration state without
changing semantics. One mutable `IndexedState` instance must not be concurrently
mutated by overlapping tock executions unless the node's own state representation
makes that safe. All tock execution is background-only. GraphExecutor may serialize, duplicate
or reset its acceleration state without introducing any audio-thread lock, live
callback, or ephemeral-output exception.

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

The name deliberately does not contain `batch`: this callback still evaluates one
node. The executor-level transaction is already batched across roots and coalesces
all work for that node into one invocation. A future `tock_coverage_batch()` would
be a different ABI optimization that evaluates **multiple nodes** simultaneously,
with separate coverage and optional acceleration state for each node.

## 11. Batched requested coverage and stored-candidate completion

Background evaluation is organized around **batched demand**, not one traversal per
read or invalid page. A batch first collects all demand roots that are allowed to
participate in that transaction, unions convergent requirements, then runs one
reverse phase and one forward evaluation phase.

One background evaluation transaction is evaluated against one coherent environment: one
target/base `(semantic_version, page_version)` pair, one sample-rate/configuration
view, one coherent set of persisted-page bindings, and—when Tick capture has pending
records—one fixed capture-sequence snapshot selected before forward propagation begins.
Captures sealed after that cutoff are not part of the transaction.
Pure stored reads may need neither R nor T; a pure invalidation
batch may defer R/T; and a mutation-plus-fetch operation may run F then R/T before
returning results. The batching invariant constrains how work is coalesced when a
phase is present, not which phases every caller must execute.

There are two closed classes of **external demand roots**:

1. **application/UI random-access fetches**, such as JSON-RPC requests for one or more
   requestable outputs/regions; and
2. **advance preparation demand** for sequential playback, scheduled in the
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
   input, or prepared bounded transient storage for a sequential input.

A request for any published persisted output reads the requested subset from the
canonical persisted-page snapshot selected for the transaction. A Tock/persisted
candidate may require background reconstruction before a successor version can
publish. Tick/persisted data reaches the same page store through the Tick-capture /
finalization path and needs neither a Tock callback nor an explicit recorder.

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
one exact forward pass (F)
        |
        v
exact changed regions + invalid tock/persisted pages
        |
        v
promote invalid pages to complete covered page domains
        |
        +-------------------------------+
        |                               |
        |                    explicit UI/preparation demand roots
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
payload belongs to the previous semantic version, so its full covered page domain
remains in the transaction and its producer is traversed. Tick/persisted candidate
pages are filled from the fixed Tick-capture snapshot instead of traversing the live
Tick producer.

Complete stored publication does **not** require every transient intermediate or
all invalid pages to coexist in memory at once. GraphExecutor may reuse bounded
transaction storage while honoring the semantic batch. Such chunking must not
split a node into multiple `tock_coverage()` invocations for the same logical batch;
if physical workspace cannot hold the node's consolidated requirements, the
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
seed affected nodes/endpoints
        |
        v
walk nodes once in forward dependency order
        |
        v
union exact changed input regions + current input coverage
        |
        v
propagate_forward_coverage() at most once for this node
        |
        +--> publish exact new computed-output coverage
        |       |
        |       `--> derive added/removed coverage as semantic changes
        |
        `--> publish exact changed output regions
                |
                +--> invalidate intersecting tock/persisted candidate pages locally
                |
                `--> union exact semantic changes into downstream accumulators
```

Fan-in never causes repeated callbacks within the batch. Every upstream path that
can reach a node in forward order has contributed to that node's per-input change
accumulator before its callback runs. Fan-out distributes the callback's consolidated
output changes into downstream accumulators.

Forward propagation continues **through** `tock/persisted` computed outputs. A stored
output is not an invalidation cut: keeping an older page payload alive does not
make downstream semantics current for a new candidate version. Page boundaries
only decide which local candidate pages become invalid; the exact semantic changed
regions continue downstream without widening.

The forward phase itself does not recursively evaluate nodes. After it finishes,
all invalid stored page domains are known and can be promoted to complete covered
page-domain demand roots for the batch's R/T phase. The executor may defer that
materialization, but a candidate containing `tock/persisted` outputs cannot become
published for its target version pair until all of its covered stored page domains
are valid.

## 13. Authored port schema, retention, and replayability

The port schema has independent input access, output production, and output
retention. Payload properties remain separate for samples and events:

```cpp
struct SequentialInputConfig {
    std::size_t history = 0;
};
struct RandomAccessInputConfig {};
using InputAccessConfig =
    std::variant<SequentialInputConfig, RandomAccessInputConfig>;

struct TickOutputConfig {
    std::size_t history = 0;
    std::size_t latency = 0;
};
struct TockOutputConfig {};
using OutputProductionConfig =
    std::variant<TickOutputConfig, TockOutputConfig>;

enum class OutputRetention { ephemeral, persisted };

struct InputConfig {
    // Existing sample/event payload properties and name/identity also apply.
    InputAccessConfig access{SequentialInputConfig{}};
};
struct OutputConfig {
    // Existing sample/event payload properties and name/identity also apply.
    OutputProductionConfig production{TickOutputConfig{}};
    OutputRetention retention = OutputRetention::ephemeral;
};
```

These are abbreviated sketches of the current public schema: `InputConfig` and
`OutputConfig` retain their sample/event payload properties and identities.
Input access, output production and output retention are independent in reflection,
configured-graph serialization and the compiler record interface. The inferred
`inward_input_access()` / `inward_output_access()` conversions and ambiguous
`is_indexed()` predicates are removed. GraphJit now classifies per-channel
compatibility and delivery using those orthogonal contracts; execution of the retained
background plan begins in step 4.

| production | retention | producer and retention semantics |
| --- | --- | --- |
| tick | ephemeral | sequential generation; no post-window retention obligation |
| tick | persisted | sequential generation; every finalized generated value is retained |
| tock | ephemeral | background demand-driven generation; transaction-local materialization permitted |
| tock | persisted | background demand-driven generation; generated covered pages retained |

`ephemeral` does not forbid transaction-local materialization. `persisted` is a strict runtime
retention guarantee, not a best-effort cache or a promise of project-file
serialization. No persisted page is evicted for memory pressure, age, inactivity,
invalidation, or lack of readers. Retained pages are removed only when output
coverage ceases to include them; old physical versions are freed only after their
pins are released. A persisted tick output becomes subject to that guarantee when
its history/latency contract makes a position final. An author opting into
persistence also opts into potentially unbounded memory use; the compiler does not
invent an eviction or recording policy to cap it. Physical backing (RAM, file,
mmap, etc.) is an implementation choice that must preserve this guarantee.

`TickOutputConfig` does not imply that its output can only be read sequentially.
Persisted tick data can be read at finalized published positions; an ephemeral tick
output may also be randomly accessible when GraphJit proves contextual replayability.
No retention or access mode is inferred from a tiled expression: each member keeps
its own contract.

### Replayable node trait

A node opts into the intrinsic replay **type trait** by declaring
`static constexpr bool intrinsically_replayable = true` (default false).
`iv::details::intrinsically_replayable_v<Node>` is the validated compiler-facing
value; it is not a port field or an additional DSP callback. An opted-in node must define the
existing `tick()` but **not** its own native `tick_block()`, have no `State`, no
random-access inputs, no input/output history, no input/output latency, and zero
internal latency. Its tick computation must be deterministic and side-effect-free
under one immutable semantic/configuration version, declared inputs, and absolute
sample position; no mutable external or live-only resource may affect its result.
Validate instance-dependent properties such as internal latency after configuration.
Event replay, if enabled, must additionally preserve deterministic timestamp/order
and declared capacity; do not silently infer event replay from the sample rule.

Existing node traits already generate the `tick_block()` wrapper for `tick()`;
GraphJit already imports its LLVM definition. Replay uses **that** imported
wrapper in the background evaluation DAG, allowing normal O3 inlining, fusion,
value specialization, and possible SIMD. It does not redefine `tick()` for ordinary
nodes or introduce a second implementation of the DSP algorithm. GraphJit can
synthesize identity-time forward/reverse propagation for the replayable node's
static pointwise dependencies, rather than require new authored callbacks.

The trait gives **intrinsic eligibility** only. An output is contextually
replayable for requested coverage only if all of its required upstream inputs are
reproducible or available in finalized published data for the selected semantic
version. A pure gain node fed by an unrecorded microphone is not historically
replayable; the same gain fed by recorded data can be. A retained published tick
output terminates traversal even if its original producer cannot replay. Replay
uses isolated invocation-local scratch, immutable configuration and a canonical
block-position/alignment contract; it cannot mutate shared live execution state.

## 14. Persisted pages share the canonical whole-graph block quantum

All persisted outputs use one canonical persisted-page store abstraction regardless
of whether their producer is Tick or Tock. Pages are canonically aligned fixed-width
units whose width matches the fixed whole-graph root block size for the active layout
generation. This is a physical representation choice; semantic coverage remains
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

Tick/persisted retention begins when positions become final under the ordinary
history/latency contract. Finalized Tick data enters the same persisted-page store
used by Tock/persisted output; no implicit recorder or Tock callback is required.
The production path may use allocator-managed capture blocks before publication, but
Random Access observes the data only through the canonical published page view in
the preliminary implementation.

Payload representation may adapt to coverage occupancy: dense pages may store one
value per physical position, while coverage-packed pages store only covered
positions. The outer page directory should keep page-index order and small stable
handles while large payloads live in stable arena/chunk/file-backed storage.

### Changing the whole-graph block size repages stored data

Changing `B` changes physical partitioning, not DSP meaning. Persisted values are
losslessly repartitioned onto the new canonical page grid during a quiescent
layout transition. Sample values retain absolute indices; event payloads are
repartitioned by absolute index while preserving deterministic event order.

Tock/ephemeral outputs have no persisted payload obligation requiring migration.
Tick-capture slabs are pre-publication/background-input storage, not a second
persisted representation. Recording and Tick/persisted staging may share the pool.
Consumed blocks become reclaimable after the transaction that consumes them commits,
subject to the callback-boundary rule for blocks visible to the audio thread. Old
immutable published snapshots may retain the old physical layout until their readers
release them.

## 15. Stored event pages, fan-in order, and random-access event iteration

Persisted event outputs use whole-page candidate validity with packed event
payloads.

A valid event page means the **complete event set for every position in the exact
covered page domain is known**, including the possibility of zero events.

`EventOutputProperties::max_events_per_index` remains density/capacity metadata,
not a literal per-timestamp quota. For one stored page:

```text
covered_positions    = measure(page_domain)
required_event_slots = ceil(max_events_per_index * covered_positions)
```

For a generated live event representation spanning `W` positions, the analogous
capacity uses the appropriate compiler-known effective fan-in bound over `W`.
When several event connections are combined into one buffer, GraphJit must account
for all incoming effective capacity bounds so every legal incoming sequence fits
without audio-thread allocation.

Background evaluation may dynamically commit persisted event payload capacity off
the audio thread. Tick-produced persisted events and explicit recording use
slab-backed capture blocks supplied by the shared capture allocator; the audio-thread
path consumes only already-provisioned blocks and performs no request-sized allocation.
A future recent-data overlay would use the same sealed event blocks as a bounded queue
in front of published event pages, but the preliminary Random Access path does not.

Event order is semantic and deterministic. Within one producer, events are emitted
in nondecreasing absolute sample-index order. Fan-in uses a stable tie break:

```text
1. absolute sample index
2. stable semantic source/connection ordinal
3. producer-local event order
```

Page evaluation order, worker scheduling, cache/store layout, and request order
must never change equal-timestamp ordering.

Because one logical random-access event read can span multiple stored pages, node-facing
reads expose a segmented ordered range/iterator rather than promise one contiguous
`std::span<TimedEvent>`. Iteration preserves the ordering above and never exposes
events outside input coverage.

## 16. Persisted output storage, stable output identity, and `NodeStorage`

Each executable generation still has one canonical fixed-layout `NodeStorage`, but
dynamically sized persisted-output data is **not** part of that fixed layout and
should not be owned merely by one JIT generation when the output has stable project
identity.

`NodeStorage` contains storage whose shape is known when the `CompiledGraph` is
built, including:

- sequential `State`;
- optional `IndexedState`, which is non-semantic `tock_coverage()` acceleration
  state only;
- compiler-owned bounded persistent regions;
- bounded reusable workspaces where useful; and
- Tick carry/history/feedback storage selected for persistent placement.

`IndexedState` is not an authoritative persisted-output store and is not shared with
`tick_block()` or propagation callbacks. A node's observable behavior must remain
correct if its `IndexedState` is discarded or independently instantiated.

`GraphExecutor` instead owns stable persisted-output storage plus
per-generation endpoint bindings. Conceptually:

```cpp
struct StableIndexedOutputId {
    StableConcreteNodeId node;
    IndexedOutputPortId output;
};

struct StoredIndexedEntry {
    StableIndexedOutputId id;
    OutputProductionConfig production;
    OutputRetention retention = OutputRetention::persisted;
    // Versioned coverage/page/root state and stable payload storage.
};
```

Ownership rules:

- every persisted output binds to one canonical persisted-page store entry keyed by
  stable output identity when that identity exists, regardless of Tick/Tock production;
- compatible executable generations rebind to that stable page-store entry rather
  than copying retained payloads;
- anonymous persisted outputs may use generation-local page-store identity;
- ephemeral outputs own no persisted-page entry to migrate or rebind; and
- Tick-capture slabs/log records are executor/runtime pre-publication inputs and are
  not a second published representation.

Persisted-storage reuse is normally **rebinding**, not copying. Page directories,
payload backends, file/mmap-backed roots if ever used, and immutable snapshots may
remain owned by the executor while old/new executable generations refer to appropriate
versions. Disk backing is not implied by `persisted`; it would merely be one backend
of the same page-store abstraction.

The currently specified physical forms are therefore:

```text
current Tick representation
canonical persisted-page store for Tick/persisted and Tock/persisted outputs
transaction-local addressable materialization for background ephemeral Random Access
prepared sequential/addressable window for ephemeral Tick-time consumption
shared Tick-capture log/pool for persistence staging and explicit recording
```

The capture log is append-only by insertion sequence while outstanding, slab-backed,
and independently provisioned from background-evaluation progress. Tick/persisted
production uses it to move finalized Tick values toward the canonical page store
without requiring a Tock callback. When layout permits, one capture block may also be
the current Tick payload read by same-Tick Sequential consumers; this is physical
coalescing of capabilities, not a second persistence format.

## 17. Background evaluation transaction workspace

Coverage planning and background evaluation need request-sized temporary storage that is not
necessarily bounded at graph compile time. `GraphExecutor` should own/reuse a
transaction workspace or arena containing things such as:

- per-node/per-port forward-change accumulators;
- per-node/per-port reverse-requirement accumulators;
- computed-output request sets used by the one-call-per-node tock pass;
- forward/reverse region-set work buffers;
- selected stored-page completion plans;
- background-produced `tock/ephemeral` result/intermediate sample/event values;
- temporary event payloads and segmented views; and
- temporary references to immutable stored snapshots/pages.

The workspace is initialized once per background evaluation transaction. All invalidation or
demand roots assigned to that batch contribute into the same accumulators before
the corresponding traversal reaches a node. Transaction-local values remain
shareable across all consumers in the same batch. Once their last consumer is
complete, storage may be reused; future liveness packing is an implementation
optimization.

Tock/ephemeral materialization and all Tock callbacks are scheduled off the audio
thread. Transaction-local addressable materializations needed by background Random
Access remain alive through their consuming evaluation; prepared Tick-time windows
survive until their callback readers release them. Tick capture is different: its
logical backlog may grow with production duration and background lag, so the capture
allocator extends slab-backed storage while the audio-thread path consumes only
already-provisioned free blocks.

## 18. Background evaluation components and cycle validation

The **background evaluation DAG** contains tock-produced dependencies,
contextually replayable tick producers and the ordinary sequential input edges
traversed while replaying them. Published persisted outputs are stored boundaries:
the background evaluator reads their selected version and does not traverse their
live producers. Weakly connected evaluation components provide forward/reverse/
evaluation orders, stable endpoint ordinals, coverage accumulators and callback
imports. The existing tock propagation callback ABI is unchanged; replayable
pointwise tick nodes use compiler-synthesized temporal propagation.

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

1. ordinary node configuration/resources and optional `IndexedState` are
   initialized;
2. current random-access-input coverages are available once upstream coverage is known;
3. `propagate_forward_coverage()` runs with a node-created/local-change cause and
   may have zero changed random-access-input regions; and
4. exact computed-output coverage is established before demand may target it.

For a newly created Tock output, old coverage is empty, so all new coverage is
an exact semantic output change and propagates downstream.

For a replayable tick output, GraphJit derives pointwise coverage from the exact
available coverage of its transitive dependencies; the node does not acquire
authored tock or propagation callbacks. A tick/persisted output acquires
random-access coverage as its finalized values are published, whether or not
its producer is replayable.

An explicit recorder output begins with whatever exact coverage its authored
semantics establish from captured records/restored retained state. Tick capture itself
does not publish output coverage. When a background transaction snapshots new
capture records, their `(OutputPortId, GlobalBlockPosition)` identities seed exact
changed/added coverage for recorder outputs. For Tick/persisted outputs, the same
fixed capture snapshot supplies finalized regions to candidate persisted pages; only
page publication extends the Random-Access-visible retained snapshot.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Compatible stored state and
coverage are rebound unless semantic configuration, random-access input connection sets,
implementation semantics, sample rate, or another explicit forward cause requires
recomputation.

A `tock/persisted` candidate must become complete over its full new coverage
before publication. A `tock/ephemeral` output has no persisted precomputation obligation.

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published coverage or persisted-output semantics enters
through an executor-controlled boundary. Code outside `GraphExecutor` must not mutate active
persisted roots/pages behind the executor's versioning rules.

The architectural invalidation-root set is deliberately closed:

- node-local semantic state/resource mutation;
- a fixed snapshot of newly sealed Tick-capture blocks (including recorder and
  Tick/persisted staging records);
- graph semantic configuration/topology/implementation change; and
- project sample-rate change.

Node-type-specific rules determine the exact initial changed regions for a
node-local mutation. Tick captures already identify their originating output port
and global block position, so the executor coalesces selected records into exact
changed coverage per output. Graph/runtime rules
determine seeds for the other classes. Once seeded, a random-access-input change at a
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
seeded together before the batch's forward pass; intermediate semantic versions
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

A recording bridge contributes captures differently from an ordinary semantic edit.
Before the background evaluation pass starts, the executor snapshots the largest currently
available contiguous prefix of capture **insertion sequence** beginning at the
capture log's unprocessed frontier. That snapshot is immutable for the pass.
Captures appended while propagation or tock is running are not added to the current
batch even if they target earlier global positions; they wait for the next pass.

The selected capture records seed ordinary forward invalidation. The bridge's
`tock_coverage()` then materializes the bridge output required by the selected
retention policy from those captured payloads as part of the same reverse/tock
transaction as downstream work.
Only the final transaction commit publishes a new page version.

External reads have a correspondingly closed entry-point set: application/UI
random-access fetches and advance background preparation for sequential playback.
The live `CompiledGraph::tick_block()` reads published/prepared data or supplies
its consuming sequential input's neutral value; it never runs tock.
Invalid `tock/persisted` page domains are internal materialization
roots, not a third external caller. The external protocol may combine updates and
fetches in one request; `GraphExecutor` normalizes that shape into mutation seeds,
demand seeds, and one or more legal background evaluation transactions.

## 21. Semantic versions, page versions, and stale-work rejection

Background computation may overlap Tick execution and may be superseded by newer
edits or by captures that arrive after a running batch has taken its snapshot. The
published page-backed output state uses two version coordinates:

- **semantic version** identifies graph/configuration/resource/sample-rate semantics;
- **page version** identifies one immutable published page view produced by
  a completed background evaluation transaction under those semantics.

A Tick capture does **not** advance the page version. Captures accumulate as pending
transaction input. A page version advances only when the background worker has
processed a fixed capture prefix, propagated the transaction's invalidation roots,
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
all finalized captured regions in the selected capture prefix; the live Tick producer
is not replayed merely to fill those pages.

For ephemeral Tock/replay results, no persisted-output validity exists; the selected
version chooses the configuration, coverage, sample rate, and immutable persisted
upstream pages against which the exact request is evaluated.

A capture-consuming pass owns one fixed executor-wide capture-sequence interval, for
example `[processed_sequence, snapshot_tail)`. The candidate may read exactly those
captured blocks plus already-published retained state. Captures at or beyond
`snapshot_tail` are invisible to that pass and cannot change its coverage,
invalidation roots, reverse requirements, or Tock/replay inputs.

If the pass commits, publication atomically advances the page version and the capture
log's processed frontier together. Consumed capture blocks then become reclaimable
subject to remaining background ownership and the root-callback boundary rule. If
the pass is cancelled or rejected as stale, its processed frontier does not advance
and its capture blocks remain available for a later pass.

## 22. Direct connection compatibility and explicit recording

The port validator checks **each source channel** against the destination input's
access contract, not equality of the producer callback and consumer callback.
These are connection permissions, not guarantees of audio-thread scheduling:

| Source output | Sequential input | Random Access input |
| --- | --- | --- |
| Tick/ephemeral, unreproducible for demanded coverage | allowed | explicit persistence/recording policy required |
| Tick/ephemeral, contextually replayable | allowed | allowed by background replay into an addressable materialization |
| Tick/persisted | allowed from the current Tick representation | allowed through the selected published persisted-page snapshot |
| Tock/ephemeral | allowed with a prepared sequential/addressable window | allowed with transaction-local materialization in background or prepared addressable data for Tick use |
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
prepared addressable window that survives through the callback. Persisted pages
follow the strict retention guarantee. Replayable tick subgraphs
may be fused and materialized only where their consumers' addressing contracts
require it. Every tock callback, forward/reverse propagation callback, and page
recomputation runs off the audio thread.

An explicit recording node is required where an unreproducible ephemeral tick
stream must supply historical random-access demand. The author selects the
recording semantics (for example an in-memory temporary buffer or file-backed
recording), including its capacity/lifetime and behavior when capture outruns
processing. GraphJit does not insert an implicit generic recorder. An independently
authored `tick/persisted` output already has an explicit persistence obligation;
its finalized published data needs no separate recorder.

### Storage requirements are inferred over overlapping endpoint subsets

Connection compatibility is checked per contribution, but **physical storage is not
chosen per connection**. A source channel can fan out to several differently
configured inputs, and only a subset of those channels may participate in another
connection. The inverse is also true for target inputs with overlapping fan-in.
GraphJit must therefore partition source and target endpoint elements into maximal
**endpoint atoms** with identical incidence before joining storage requirements.

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
full requirements later prove equivalent may be physically coalesced. Conversely, a
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
    require prepared sequential window

if an ephemeral Tock/replay result has any Tick-time Random Access use:
    require prepared immutable addressable window

if an ephemeral Tock/replay result has background-only Random Access use:
    require transaction-local addressable materialization

if S is unreproducible Tick/ephemeral and any Random Access demand reaches it:
    reject implicit storage; require authored persistence/recording
```

These requirements form a capability join, not a single strongest storage enum. A
prepared addressable window can also provide sequential slices for the same range,
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
capture blocks do not extend the selected view's coverage.

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

### Tick capture records and explicit recorder bridges

The shared Tick-capture transport accepts any Tick-produced payload whose lifetime
must escape ordinary current-block execution. Explicit recorder bridges use it for
otherwise unreproducible sequential data; Tick/persisted outputs use it to stage
finalized data for canonical page publication. Capture occurs at the producer/
finalization point, not at the end of the whole root callback. When layout permits,
the producer writes directly into a pre-provisioned capture block; otherwise the
generated path performs a bounded copy. The block is sealed as one immutable record:

```cpp
struct CapturedBlock {
    CaptureSequence sequence;
    OutputPortId output_port;
    GlobalBlockPosition position;
    // immutable payload owned by capture storage
};
```

`sequence` is the monotonic insertion order of the executor's shared Tick-capture
log. All participating capture-backed outputs share that ordering domain. It does
not order timeline positions. `output_port` identifies the output whose retained or
recorded value/coverage is changed by the payload. Consecutive records may belong to
different outputs, and seeking while playback/recording is active may append records
for positions earlier than records that have not yet been consumed by background
evaluation.

For example:

```text
sequence     output port     global position
100          A               1200
101          B               1200
102          A               1216
103          A                400   <- seek
104          B                400
105          A                416
```

This is one contiguous capture prefix `[100,106)` even though neither port identity
nor global position is contiguous. If multiple captures in one snapshot overlap the
same output/range, bridge materialization applies them in capture-sequence order so
later captures represent the later recorded state.

### Fixed capture snapshot per background evaluation pass

At the beginning of one background propagation/Tock pass, the worker reads the
current capture tail exactly once and fixes a cutoff:

```text
begin   = executor capture log's first unprocessed sequence
cutoff  = capture tail published when the pass begins
batch   = [begin, cutoff)
```

That batch is immutable. A capture appended after `cutoff` belongs to the next pass
even if its `GlobalBlockPosition` is numerically earlier than positions in the
current pass.

The worker converts the fixed records into changed/added `IndexedCoverage` keyed by
`OutputPortId`. Those output changes are ordinary invalidation roots. From
that point forward there is no bridge-specific downstream scheduler: normal
`propagate_forward_coverage()` determines downstream changed coverage, normal
reverse planning determines the exact input coverage required by implicated nodes,
and normal `tock_coverage()` evaluation materializes bridge output pages and all
affected downstream outputs.

A bridge's captured payload is transaction-input storage, not a published output
view. Coverage/Tock callbacks never observe captures outside the fixed snapshot, and
capture insertion never changes a running transaction's inputs.

## 23. Complete persisted outputs and physical backing

The persisted-page store is canonical for both Tick/persisted and Tock/persisted
outputs. A candidate successor is never exposed as a partially complete published
snapshot. Tock/persisted completion may require its entire exact candidate coverage
to be materialized before publication; Tick/persisted pages become eligible as their
positions become final under the Tick history/latency contract and are incorporated
into a coherent successor page version.

Complete materialization does not require all bytes to remain in anonymous RAM.
Physical backing may use mmap/files, compression, deduplicated immutable pages,
or copy-on-write roots, but **never automatic eviction** of persisted covered
data. Invalidation retains readable old published pages while a successor is
computed. Pages may be removed from the logical retained output only when its
coverage no longer includes them. Superseded immutable physical versions may be
reclaimed after their readers release them. Any representation accessed directly by the audio-thread Tick path must meet
audio-thread access requirements.

Tick/persisted output differs only in **how persisted pages are populated**:
`tick_block()` may revise positions under normal history/latency semantics, and only
finalized positions enter the persistence pipeline. Persistence does not impose
whole-block-or-none authoring semantics or require Tock production. Once published,
Tick- and Tock-produced persisted data use the same page lookup, versioning, pinning,
and consumer read path.

## 24. Tick capture allocation, snapshots, and reclamation

Capture-block provisioning is shared infrastructure for Tick-produced data whose
lifetime must escape ordinary current-block execution. Explicit recording bridges
use it to make otherwise unreproducible sequential data available to background
work. Tick/persisted outputs may use the same pool/log to stage finalized Tick data
on its way into the canonical persisted-page store.

Three independently progressing runtime roles prevent arbitrary background latency
from having to fit inside a fixed audio-thread staging window. Capacity is allocator
managed rather than defined as a fixed number of seconds; when capture production
stops, the background worker can drain the remaining backlog.

```text
capture allocator
        |
        | allocate/recycle slabs
        v
pre-provisioned free capture blocks
        |
        | audio thread is the consumer
        v
Tick output production/finalization during tick_block()
        |
        | direct fill or bounded copy + metadata + seal
        v
append-only Tick capture log
        |
        | fixed sequence snapshot per pass
        v
background evaluation / page-publication worker
        |
        | optional F/R/T work + atomic page-version commit
        v
published page version + capture blocks reclaimed or transferred to page ownership
```

### 24.1 Audio-thread capture

The audio-thread path does not wait for background evaluation and does not scan the
graph at the end of `tick_block()`. At the precise point where a recording block or
persistable Tick block becomes eligible for capture it:

1. consumes one already-provisioned free capture block (or produces directly into
   such a block when the selected physical layout permits it);
2. copies/finalizes the sample/event payload as needed;
3. writes the stable output identity, global block position, and next capture
   sequence; and
4. seals/publishes the immutable record to the Tick capture log.

Only the audio-thread path consumes blocks from the free-capacity pool. The allocator
never takes a free block back from underneath it. A capture block that participated
in the current root callback remains stable through that callback boundary even if
background work determines earlier that it is otherwise reclaimable.

### 24.2 Slab provisioning

A dedicated capture allocator keeps free capture capacity near a configured target
for the shared Tick-capture pool/log. It allocates append-only slabs in units large
enough to amortize allocation cost but small enough to provision promptly,
splits/reuses them as capture blocks, and publishes those free blocks for audio-thread
consumption. Recording and Tick/persisted staging may draw from the same pool; their
semantic policies differ, not their need for pre-provisioned audio-thread-safe blocks.

Provisioning is independent of background evaluation. If the background DAG takes
four seconds, forty seconds, or longer, sealed blocks may accumulate in ordinary
slab-backed capture storage while the allocator continues extending capacity. Slow
background execution therefore does not by itself force block dropping or a fixed
handoff overrun.

No finite reserve can provide a mathematical no-allocation/no-block/no-drop
guarantee if the allocation worker itself is prevented from running indefinitely
or the process exhausts addressable/storage resources. That catastrophic policy is
separate from normal bridge semantics; the architecture must not impose ordinary
block loss merely because background evaluation is slower than audio-thread production.

### 24.3 Background consumption

The background worker never races the capture tail during a pass. It snapshots the
largest momentarily available capture-sequence prefix at pass start and processes
exactly that prefix. Newly visible blocks are left for the next pass.

For the fixed snapshot it:

1. derives exact changed/coverage roots per recorded output port;
2. runs the ordinary forward invalidation pass;
3. promotes invalid tock/persisted page domains as usual;
4. runs ordinary reverse planning;
5. runs ordinary background evaluation: `tock_coverage()` for implicated tock
   producers and imported generated `tick_block()` for eligible replayable
   producers; and
6. publishes one coherent successor page version for the complete transaction.

This is the normal background evaluation routine. There is no intermediate
"capture-pages publication" that downstream work waits for.

### 24.4 Reclamation

The published persisted-page abstraction owns the retained result. A background
transaction may copy from capture storage or, when page/capture payload geometry and
ownership permit it, adopt the payload into the page store without changing the
consumer abstraction.

Successful publication makes the consumed capture records logically obsolete. If
the candidate copied their payload, a physical capture block returns to the allocator
only after all background ownership is gone **and** no audio callback that could have
observed it is still active. If the persisted-page store adopted the payload, physical
ownership transfers to that page version and the block is not returned to the capture
free pool until that page's physical lifetime ends. In particular, no block visible
to a root `tick_block()` callback is handed back to the audio-thread free pool during
that callback. A failed/cancelled/stale transaction cannot advance its capture
frontier; those records remain pending until a later transaction commits them.

The preliminary Random Access path still reads only published pages, not recent
capture blocks. Keeping capture payloads stable through callback boundaries is a
prerequisite for the later same-Tick recent-overlay experiment, not a claim that the
overlay already exists.

## 25. Random-access event/sample reads from node callbacks

Random-access sample reads address global covered sample positions. Tick/persisted
and Tock/persisted outputs use the same published persisted-page read path. In the
preliminary implementation, a Tick callback pins that published snapshot at the
callback boundary; current Tick buffers, pending candidates, and recent sealed
capture blocks are not alternate lookup sources.

Ephemeral Tock/replay results use immutable addressable materializations instead of
persisted pages. Background-only consumers may use transaction-local materialization.
A Random Access input used during Tick execution requires the needed ephemeral result
to have been prepared into an immutable addressable window before the callback.

Random-access event reads may span several stored segments/pages and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Random-access accessors expose exact input coverage. Debug validation catches
requests outside coverage. A `tock_coverage()` accessor reads Random Access inputs from the selected immutable
transaction view, which may include canonical persisted pages regardless of Tick/Tock
provenance, background-replayed Tick values, transaction-local Tock/replay
materializations, and an explicit recorder's published output. It never reads mutable
current Tick storage or capture records appended after the transaction snapshot
cutoff.

## 26. External random-access result lifetime

Application/UI callers should not receive unscoped raw pointers into executor-owned
immutable storage whose snapshot may later be reclaimed.

Two valid host-side result models are:

- return owned/copy results; or
- return an explicitly scoped/pinned immutable view that keeps the referenced
  stored snapshot/representation alive for the view lifetime.

The simplest initial application boundary should prefer owned results. Generated
internal tock/reverse execution may use borrowed transaction/stored views whose
lifetime is controlled by `GraphExecutor`.

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
exact changed IndexedCoverage
```

Notification is emitted from the executor publication/change boundary, not
recursively from individual callbacks. Presentation/UI code can react by issuing
ordinary random-access requests. The notification does not itself force
`tock/ephemeral` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and semantic invalidation are separate events. Producing a new
`CompiledGraph` generation does not by itself make any stable persisted output
stale or revoke its retention guarantee.

`GraphExecutor` reconciles old/new generations using stable concrete-node/output
identity. Compatible persisted outputs rebind to the existing stable store rather
than copying payloads merely because machine code changed.

Important cases:

- tock/persisted with unchanged semantics preserves coverage and payloads;
- tock/persisted with changed semantics runs exact invalidation and rebuilds
  invalid candidate pages before publication;
- tick/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no persisted payload to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic endpoint identity rather than builder
handles, lowered node indices, or ORC-generation-local ordinals.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output Tick root. It does
not gain synthetic output ports for background evaluation or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal coverage-planned endpoints to
generated background-evaluation component executors, persisted bindings,
advance-preparation plans,
and explicit recording-bridge capture/output identities. Static background-evaluation topology is
specialized during lowering rather than rediscovered for every request or block.

### Reuse whole-graph analysis products during lowering

Whole-project lowering reuses one SCC decomposition of the complete semantic
dependency graph to reject random-access input edges within a semantic SCC.
Separately, expand the background evaluation DAG through sequential input edges
of replayable tick nodes. Reject any unresolved replay dependency cycle; a valid
published persisted output is a terminal stored boundary for its covered requests.
Checking only labeled random-access edges is **not** sufficient for this second
check. Reuse the same stable ordinals and graph-analysis data; do not perform a
new reachability search per output.

SCC IDs, member ranges, condensation-DAG/topological order, dense node ordinals,
and reusable adjacency storage should be carried forward where useful. Background
component planning, same-Tick scheduling, liveness/storage planning, access/retention
lowering, and later batching/fusion/vectorization should consume those facts
instead of rebuilding equivalent graph views.

This is an efficiency rule, not a semantic requirement to collapse distinct graph
relations into one. Explicit-DAG validation, detach legality, same-Tick
dependencies, and complete semantic dependency relations answer different
questions. Detach validation may still require pre-feedback reachability. Share
ordinals/storage/traversal scratch where the edge relation actually matches.

The initial compiler should recompute SCCs from scratch for each graph compilation
rather than implement dynamic SCC maintenance. A complete Tarjan/Kosaraju-style
pass is linear, simple, deterministic, and expected to be negligible beside LLVM
lowering/optimization until profiling proves otherwise.

`GraphExecutor` owns:

- canonical fixed-layout `NodeStorage` for each retained executable generation;
- stable canonical persisted-page stores/immutable roots for **all persisted
  outputs** (`tick/persisted` and `tock/persisted`), plus per-generation bindings;
- semantic versions plus immutable page versions and candidate/published
  snapshots;
- the shared Tick-capture log/pool used by explicit recording and Tick/persisted
  staging, its processed-sequence frontiers, callback-epoch ownership, slab/block
  reclamation state, and coordination with the capture allocator;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/Tock transactions;
- completion scheduling for candidate Tock/persisted outputs and fixed-prefix
  incorporation/publication of Tick/persisted captures; and
- external Random Access request/change-notification lifetime.

## 30. Deliberately open implementation/tuning choices

Implementation choices include physical page backing, snapshot directories,
transaction-workspace reuse, capture slab sizes and queue/watermark strategy,
worker scheduling, cancellation granularity, and post-correctness SIMD/fusion/
value-specialization cost models. None of these may weaken the authored persisted
retention obligation or introduce audio-thread tock execution.

The following are **not** implementation choices: static constexpr concrete node
port schemas; independent input access/output production/output retention; explicit
opt-in replayability with contextual dependency validation; no implicit recording of
unreproducible Tick/ephemeral sources; one canonical persisted-page read abstraction
for Tick/persisted and Tock/persisted outputs; background-only Tock and propagation;
published/prepared-snapshot-only Tick-time Random Access in the preliminary
implementation; stale-page-as-is/missing-page-per-input-neutral Sequential playback;
subset-first storage requirement inference before physical coalescing; no persisted
page eviction except loss of covered positions; exact coverage and forward changes;
value-blind reverse demand; fixed capture-prefix transactions; callback-boundary-safe
capture-block reuse; and no unresolved Random Access evaluation cycles.

## 31. Static validation

Node/package validation rejects non-static or non-constexpr concrete port schemas
at **every** construction path (including internal builder paths), malformed
`IndexedState`, conflicting callback/production declarations, invalid event
capacity declarations, and invalid replayability trait declarations. Replayable
nodes require `tick()` without native `tick_block()`, no `State`, no random-access
inputs, zero port history/latency and zero internal latency; their replay contract
also asserts pure deterministic evaluation under a fixed version.

Whole-project GraphJit validation resolves per-channel connections and semantic
SCCs, derives contextual replayability and exact available coverage, rejects
unreproducible tick/ephemeral -> random-access demand unless an explicit recorder
intervenes, and rejects unresolved background evaluation cycles. A Tock ->
Random Access consumer requires an immutable addressable materialization; persisted
outputs use canonical pages, while ephemeral storage lifetime depends on the
consumer callback context. Explicit recorder bridges must have their valid authored
input/output shape; any Tick-capture-backed plan must have audio-thread-safe
pre-provisioned capacity and callback-boundary-safe reclamation. Unsupported event-
replay contracts must be rejected, not silently treated as sample replay. Errors
identify the specific source channel, input, dependency
path, or offending node where practical.

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
   boundaries, reject live sources and unresolved cycles, synthesize pointwise
   F/R propagation and use the imported `tick_block()` wrapper for replay.
   Preserve semantic SCCs separately from the expanded background evaluation DAG,
   enforce the recorder boundary and keep Tock execution off the audio thread.
4. **Implement storage-requirement inference and ordinary background evaluation.**
   Partition overlapping source/target subsets into endpoint atoms, join capabilities
   across all fan-in/fan-out uses, and physically coalesce only after correctness
   requirements are known. Implement `GraphExecutor`, transaction-local/advance
   ephemeral materialization, canonical persisted pages for Tick and Tock persisted
   outputs, pinned **published-snapshot-only** Random Access reads, atomic publication,
   and advance preparation. Verify that no audio-thread path can invoke Tock.
5. **Make capture-backed Tick persistence and explicit recording operational.**
   Define the shared capture ABI/pool; implement independent capture-allocator slab
   provisioning and audio-thread-safe capture at production/finalization time. Use it
   for Tick/persisted staging and explicit recorder bridges as appropriate. Consume
   fixed capture-sequence snapshots through the background transaction, publish into
   the canonical page store, and reclaim only with callback-boundary-safe ownership.
   The recent-capture Random Access overlay remains a later optional experiment.
6. **Finish generation reconciliation, authored-node cases and optimization.**
   Rebind compatible persisted output stores, preserve correct state/layout
   transitions and finish remaining node/feedback/event semantics. Only then refine
   SIMD/fusion, page placement, transient reuse and immutable-value specialization.

A capture sequence is an insertion order, not a global timeline order: seeking may
append changes at previously processed positions. The processed frontier advances
only on a successful transaction commit.

## 33. Summary invariants

1. Concrete GraphJit nodes have static constexpr port schemas; only graph topology
   and instance/connection composition are dynamic.
2. Input access (`SequentialInputConfig`/`RandomAccessInputConfig`), output
   production (`TickOutputConfig`/`TockOutputConfig`) and output retention
   (`ephemeral`/`persisted`) are independent declarations.
3. `tick()` already lowers to an imported, optimizable generated `tick_block()`.
   A separately declared node trait establishes intrinsic replay eligibility;
   GraphJit proves contextual replayability through available upstream coverage.
4. An unreproducible Tick/ephemeral source cannot directly satisfy Random Access
   demand. Tick/persisted, replayable Tick, and either Tock output can. Persisted
   outputs use canonical pages; ephemeral Tock/replay results use immutable
   addressable materialization with transaction or prepared-window lifetime according
   to the consumer callback context.
5. Tiling preserves per-channel contracts and creates neither an implicit recording
   policy nor an implicit producer.
6. `tock_coverage()` and authored propagation callbacks run only off the audio
   thread. A sequential input plays a stale published page as-is and substitutes
   **its own** `neutral_value` for a missing page without waiting.
7. Persisted outputs retain all generated/finalized covered data without automatic
   eviction. Coverage removal alone ends the semantic retention obligation;
   superseded physical versions are reclaimed only after readers release them.
   Memory growth is author-selected.
8. Exact `IndexedCoverage` and forward changes are independent of the physical
   page grid. Reverse demand is value-blind; replayable pointwise tick propagation
   is synthesized from static dependencies.
9. The background evaluation DAG has no unresolved cycles; feedback in the
   same-Tick scheduling graph retains its separate temporal semantics.
10. Tick capture uses allocator-managed pre-provisioned slabs, monotonic capture
    *insertion* sequences and fixed-prefix background consumption. Explicit recording
    and Tick/persisted staging may share those blocks. Capture insertion is not page
    publication; the preliminary Random Access path sees new Tick/persisted data only
    after canonical page publication. Audio-visible blocks are not recycled during a
    root Tick callback.
11. `IndexedState` is non-semantic Tock-only acceleration; canonical persisted-page
    stores and capture backlogs are executor-owned sidecars, distinct from fixed
    `NodeStorage`.
12. Storage requirements are joined over exact overlapping endpoint atoms, not chosen
    independently per connection or promoted wholesale per authored port. Physical
    coalescing happens only after those correctness requirements are known.
13. The legacy generated-node graph executor and dynamic concrete-port fallbacks
    are deleted; the package/configuration JIT and GraphJit's imported concrete-
    node LLVM remain.

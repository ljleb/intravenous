# Indexed DSP ports and incremental indexed evaluation

This document is the normative design for **indexed DSP ports**, their
incremental evaluation model, and the executor-side storage/publication rules
needed to make indexed data usable by both UI-style random access and realtime
processing.

`indexed` replaces the older `compiled` DSP-port terminology. `compiled` is now
reserved for actual program/JIT compilation (`GraphJit`, `CompiledGraph`, LLVM
modules, generated code, and similar concepts). The public port declarations,
callback contexts, node traits, indexed state, compiler records, and GraphJit
implementation metadata use indexed terminology directly; the former
compiled-port API is not retained as a compatibility layer.

The intended callback vocabulary is:

```cpp
tick_block(...);                       // sequential realtime execution
tock_coverage(...);                    // one-node indexed evaluation over coverage
propagate_forward_coverage(...);       // one-node changed inputs/state -> outputs/coverage
propagate_reverse_coverage(...);       // one-node required outputs -> required inputs
```

The `_coverage` callbacks each operate on **one node** even when the supplied
`IndexedCoverage` contains many disjoint regions. This deliberately differs from
existing names such as `tick_block_batch`, where `batch` means that multiple
nodes are processed as one compiler/runtime operation. A future multi-node
indexed interface may therefore use the names:

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

> A random-access input may consume a tock output (including ephemeral temporary
> pages), a persisted tick output's finalized published data, or a contextually
> replayable tick output. An unreproducible ephemeral tick output cannot directly
> satisfy random-access demand: the DSP author must place a recording node with an
> explicit retention policy. The restriction prevents **implicit recording**, not
> a technically impossible connection.

> Tiling is a non-materializing composition of source channels. It preserves each
> source's production, retention, and effective access capabilities. A whole-tile
> random-access connection requires every selected source channel to satisfy the
> requested random-access contract; channel projection retains its own capabilities.

> Computed tock outputs publish exact finite `IndexedCoverage`. Forward
> propagation records exact semantic coverage/change without forcing evaluation;
> reverse propagation is value-blind and may conservatively over-request. Page
> boundaries never widen semantic coverage or forward changed regions. A replayable
> pointwise tick node's temporal propagation may be synthesized by GraphJit.

> Indexed invalidation and demand are batched transactions: all roots accumulate
> before traversal; fan-in/fan-out requirements are unioned; each applicable authored
> forward/reverse/tock callback runs at most once per implicated node per batch.
> `IndexedState` remains non-semantic acceleration available only to tock.

> No unresolved random-access computation dependency may participate in a cycle.
> Replayable tick subgraphs must be acyclic when expanded for background demand;
> valid published persisted values can terminate replay traversal. Ordinary
> sequential feedback retains its existing scheduling semantics.

> Persisted output pages are **never evicted** for age, cache limits, memory
> pressure, or invalidation. Retained generated pages may be removed only when
> output coverage no longer includes them; replacement physical versions are
> reclaimed only after their readers release them. Unbounded memory use is an
> explicit consequence of the author's persistence declaration.

> An explicit recording node captures otherwise unreproducible sequential data at
> its production point into already-provisioned slab-backed storage. Each background
> pass snapshots a fixed capture-sequence prefix; captures enter ordinary exact
> invalidation, reverse planning, background computation, and one atomic pages-version
> publication. The capture log is temporary transaction input, not persisted pages.

This is an incremental indexed-evaluation model integrated with realtime graph
execution, not a second realtime scheduler and not a storage class.

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
random-access DAG. It does not make all `tick()` implementations pure or replayable.
No `tock_coverage()` call occurs on the audio thread.

## 2. `IndexedCoverage` is the only indexed domain boundary

Every tock output publishes a finite **coverage**: a canonical union of
nonempty, disjoint half-open global index regions. There is no separate indexed
`extent`, bounding hull, or implicit infinite default-valued domain.

For example:

```text
[0, 480000) U [4800000, 5280000)
```

may describe two audio clips separated by a large empty timeline gap. The gap
requires no indexed cache storage, validity metadata, reverse demand, or tock
work.

Outside coverage, the output is not requestable by node code. The executor and
node wrappers should assert this contract in debug builds. This is stronger than
saying that reads outside coverage return zero/no-events: lowering is allowed to
assume such reads never occur and optimize accordingly.

Disconnected indexed inputs have empty coverage.

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
    // Indexed requests made through this input stay inside coverage.
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

The executor distinguishes several concepts for persistent indexed outputs:

- **coverage**: exact regions where the output exists and may legally be requested;
- **page domain**: for one physical stored page, `page_interval & coverage`;
- **validity**: whether that whole page domain is current for a candidate indexed
  version pair;
- **payload**: retained sample/event values for the page domain;
- **semantic version**: the dependency/configuration environment for which those
  values are meaningful; and
- **pages version**: the immutable published indexed-data view against which those
  values were computed or read.

Coverage is exact and independent of physical page boundaries. Persistent stored
pages use the same canonical quantum as the fixed whole-graph root block for the
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

Physical RAM residency is intentionally not part of indexed semantics. A complete
stored output may later use ordinary heap memory, stable arenas, mmap-backed files,
compressed backing, or another representation. Such choices must still satisfy
the realtime-read requirements of the generated graph, for example by pinning or
prefaulting live regions where the backing mechanism can otherwise fault or block.
They do not reintroduce semantic "missing pages" into a published stored output.

## 5. Sparse logical requests and stored materialization

External indexed requests may be sparse. UI waveform rendering may ask for a few
positions from a very large coverage, for example. Sparse demand behaves according to the output's access and retention semantics, not according to a generic cache policy.

A `tock/ephemeral` output owns no persistent materialization. A sparse request is
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
`(semantic_version, pages_version)` pair and may become publishable.

A tick/persisted output retains finalized sequentially produced values; its
published coverage is a random-access **stored boundary** without requiring tock
or an additional recording node. A replayable tick/ephemeral output can instead
satisfy random-access demand by recomputing from available upstream values. Only
an unreproducible tick/ephemeral source requires explicit recording before such
demand. Semantic changes to any published retained source seed downstream exact
invalidation; they do not erase old readable pages.

Thus stored-page width is **not** an independent indexed-storage tuning knob. It is
the active whole-graph root block size. Choosing a smaller or larger root block
therefore changes both realtime scheduling granularity and stored-page granularity,
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

Forward propagation is independent of indexed access and is not limited to
incoming indexed-edge changes.

The executor has a closed set of **external invalidation roots**. Every persistent
indexed semantic invalidation originates from one of these classes:

1. **node-local semantic mutation**: an application/UI operation changes node
   state, configuration, or a resource according to that node type's own semantic
   rules;
2. **finalized persisted tick publication**: newly finalized or legally revised
   persisted tick positions update published retained coverage/value versions and
   seed exact changes downstream without rerunning their tick producer;
3. **recording-bridge capture snapshot**: a fixed prefix of newly captured blocks changes one or more explicit bridge indexed outputs at their recorded global positions;
4. **graph semantic configuration change**: node creation/removal/replacement,
   indexed connection-set changes, or another graph/configuration change that
   changes indexed dependencies or implementation semantics; or
5. **project sample-rate change**: computed indexed semantics are reevaluated
   under a new sample rate.

An indexed input changing because an upstream output changed is **not** another
root class. It is the ordinary continuation of the same forward batch through an
indexed connection. Likewise, creation of a new computed node is handled as a
graph semantic configuration change whose old output coverage is empty.

Executable regeneration by itself is not an invalidation root. A compatible new
`CompiledGraph` generation rebinds stable stored indexed state; only a semantic
change exposed while reconciling the generation enters one of the root classes
above.

A node may therefore run its forward-coverage logic with **zero changed indexed
input regions** because local semantic configuration changed. The callback/context
must distinguish such a local-state-triggered update from the absence of work.

A forward update may change two independent things for each computed indexed
output:

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

> Given this node's current indexed-input coverages, exact changed indexed-input
> regions, current semantic configuration, sample rate, and any local semantic
> change cause, what are the resulting computed-output coverages and which exact
> output regions may have changed?

Conceptually:

```text
F(node): input coverage + changed input regions + semantic configuration + sample rate
      -> output coverage + affected output regions
```

It runs in forward graph direction. Incoming changed regions from all indexed
inputs and all invalidation roots belonging to the batch are accumulated/unioned
before the node is visited. **Each implicated node is visited at most once by the
forward phase of one indexed batch.** A node may still have many disjoint changed
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

> Given exact covered regions of this node's computed indexed outputs that must be
> evaluated, which covered regions of its indexed inputs must be available to
> compute them?

Conceptually:

```text
R(node): requested output coverage + semantic configuration + sample rate
      -> required input coverage
```

It runs in reverse graph direction. Requirements reaching a node through multiple
downstream paths and demand roots are accumulated/unioned before the node is
visited. **Each implicated node is visited at most once by the reverse phase of
one indexed batch.**

For `tock/ephemeral`, the output requirement is the exact covered demand. For
`tock/persisted`, candidate completion first selects the complete covered domains of
invalid stored pages, and those selected domains become the requirements supplied
to reverse planning.

Persistent indexed data is a reverse-propagation boundary only when it is valid
for the `(semantic_version, pages_version)` pair selected by the batch:

- a valid `tock/persisted` page/domain for the selected target/base version pair
  satisfies that requirement and stops reverse propagation through that region;
- an invalid or nonexistent `tock/persisted` candidate page does **not** stop reverse
  propagation merely because an older physical page still exists. Its complete
  covered page domain becomes a materialization requirement and reverse planning
  continues through its producer; and
- `tock/ephemeral` owns no persistent result and therefore never forms a persistent
  reverse cut.

Thus page retention and page semantic validity are deliberately distinct. Never
dropping old immutable page payloads does not make those payloads current for a
new semantic version.

After the callback reports input requirements, every requirement is clipped to the
input's exact coverage.

The callback is **value-blind**. It may inspect coverage, semantic node
configuration, sample rate, and other deterministic structural metadata, but it
must not inspect indexed input payload values to discover a second-stage
dependency footprint. If exact addressing depends on an indexed control signal,
the callback returns a conservative superset derivable without reading that
signal, up to the complete potentially relevant input coverage. A future staged
value-dependent dependency facility may relax this rule without changing the
initial ABI.

The callback may be omitted where the conservative exact rule is mechanically
known, for example by requiring all covered regions of every indexed input.
Output-only indexed sources have nothing upstream to request and therefore require
no reverse callback.

For a tock-produced output needed by realtime playback, reverse planning and
all required computation run off the audio thread. The audio thread only reads
the already published/prepared result; an unavailable page uses the sequential
consumer's `neutral_value`.

`IndexedState` is **not** visible here. Dependency requirements may not depend on
memoization history.

## 10. `tock_coverage()`

`tock_coverage()` is the one-node indexed evaluation callback for outputs whose
output production is `TockOutputConfig`. Its context carries requested
`IndexedCoverage` per computed indexed output; one invocation may therefore
compute many disjoint regions and several outputs of the same node.

Within one indexed batch, reverse planning first finishes accumulating the final
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
- cannot depend on sequential realtime `State`;
- may use `IndexedState` solely as non-semantic acceleration/memoization state; and
- must produce observable results independent of indexed request order and the
  contents/history of `IndexedState`.

`IndexedState` has one deliberately narrow meaning:

> It may change the pace or implementation strategy of `tock_coverage()`, but it
> may not change output values, output coverage, reverse requirements, or any other
> observable indexed semantics.

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

## 11. Batched indexed demand and stored-candidate completion

Indexed evaluation is organized around **batched demand**, not one traversal per
read or invalid page. A batch first collects all demand roots that are allowed to
participate in that transaction, unions convergent requirements, then runs one
reverse phase and one forward evaluation phase.

One logical indexed batch is evaluated against one coherent environment: one
target/base `(semantic_version, pages_version)` pair, one sample-rate/configuration
view, one coherent set of tock/persisted bindings, and—when a recording bridge
has pending captures—one fixed capture-sequence snapshot selected before forward
propagation begins. Captures published after that cutoff are not part of the batch.
Pure stored reads may need neither R nor T; a pure invalidation
batch may defer R/T; and a mutation-plus-fetch operation may run F then R/T before
returning results. The batching invariant constrains how work is coalesced when a
phase is present, not which phases every caller must execute.

There are two closed classes of **external demand roots**:

1. **application/UI indexed fetches**, such as JSON-RPC requests for one or more
   indexed outputs/regions; and
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
   and contextually replayable tick nodes until reaching valid published persistent
   boundaries or source nodes;
4. evaluates implicated nodes in forward dependency order, at most once per node;
   and
5. returns/forwards the requested materialization from caller, transaction, direct
   consumer, temporary addressable pages for a random-access input, or prepared
   bounded transient storage for a sequential input.

A request for a published `tock/persisted` output does not trigger partial
reconstruction of that output. It reads the requested subset directly from the
complete persistent representation selected for the batch. Finalized published
tick/persisted data is also directly requestable over its retained coverage,
without a tock callback or an explicit bridge.

### `tock/persisted` candidate completion

Forward invalidation may create a candidate version with invalid stored page
domains. Before reverse planning, the executor promotes every invalid stored page
to its complete covered page domain:

```text
materialization_requirement(page) = page_interval & candidate_output.coverage
```

Those complete page domains are then unioned with all other materialization demands
for the same indexed batch. To make the candidate publishable, the executor:

1. gathers all invalid/nonexistent `tock/persisted` page domains that the candidate
   requires;
2. unions those internal roots with any explicit demand roots allowed in the same
   transaction;
3. runs one reverse-order pass, coalescing downstream requirements before each
   node and stopping per-region at persistent data valid for the selected semantic
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

A valid tock/persisted page or finalized published tick/persisted region may
satisfy an upstream requirement without further traversal. An invalid candidate stored page cannot: its older retained payload is
only data for an older semantic version, so
its full covered page domain remains in the batch and its producer is traversed.

Complete stored publication does **not** require every transient intermediate or
all invalid pages to coexist in memory at once. GraphExecutor may reuse bounded
transaction storage while honoring the semantic batch. Such chunking must not
split a node into multiple `tock_coverage()` invocations for the same logical batch;
if physical workspace cannot hold the node's consolidated requirements, the
executor/lowering must provide a representation or bounded streaming contract that
still preserves the one-callback-per-node batch semantics.

## 12. Batched forward invalidation phase

A forward invalidation phase begins from the closed invalidation-root set in
section 7. All roots assigned to one logical indexed batch are installed before
forward traversal starts.

The evaluator conceptually performs:

```text
all node-state / captured-bridge / graph / sample-rate invalidation roots
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

These are **target API sketches**, not claims that the current source already uses
these names. The existing source still carries `IndexedProducer` and the older
realtime/indexed port config names until the implementation migration lands.
`InputConfig` / `OutputConfig` above elide existing non-access fields rather than
replacing their real layout. The same separation must survive reflection,
configured-graph serialization, compiler records, and GraphJit planning. Generic
`inward_input_access()` / `inward_output_access()` conversions and ambiguous
`is_indexed()` tests cannot infer one axis from another and should be deleted.

| production | retention | producer and retention semantics |
| --- | --- | --- |
| tick | ephemeral | sequential generation; no post-window retention obligation |
| tick | persisted | sequential generation; every finalized generated value is retained |
| tock | ephemeral | background demand-driven generation; temporary page materialization permitted |
| tock | persisted | background demand-driven generation; generated covered pages retained |

`ephemeral` does not forbid temporary page storage. `persisted` is a strict runtime
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

Introduce an explicit node **type trait** (provisionally
`is_replayable_node_v<Node>`, default false) and validate its declaration. It is
not a port field or an additional DSP callback. An opted-in node must define the
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

## 14. Stored pages share the canonical whole-graph block quantum

Persisted tock-output storage uses canonically aligned fixed-width pages whose width
matches the fixed whole-graph root block size for the active layout generation.
This is a physical representation choice; semantic coverage and realtime
history/latency remain independent of page boundaries.

For a tock/persisted output:

```text
page_interval(i) = [i * B, (i + 1) * B)
page_domain(i)   = page_interval(i) & output.coverage
```

A candidate page becomes valid only after its complete nonempty `page_domain` has
been materialized for the target `(semantic_version, pages_version)` pair. One
`tock_coverage()` call may
cover many selected pages; page boundaries do not imply one callback per page.

Tick/persisted retention begins when positions become final under the ordinary
history/latency contract. Those finalized published positions directly satisfy
random-access demand; no implicit bridge or tock callback is required. Its physical
retention representation may differ from canonical tock pages, but requests must
observe coherent published coverage and the same no-eviction guarantee.

Payload representation may adapt to coverage occupancy: dense pages may store one
value per physical position, while coverage-packed pages store only covered
positions. The outer page directory should keep page-index order and small stable
handles while large payloads live in stable arena/chunk/file-backed storage.

### Changing the whole-graph block size repages stored data

Changing `B` changes physical partitioning, not DSP meaning. Persistent values are
losslessly repartitioned onto the new canonical page grid during a quiescent
layout transition. Sample values retain absolute indices; event payloads are
repartitioned by absolute index while preserving deterministic event order.

Indexed/ephemeral outputs have no semantic persistent payload requiring migration.
Capture slabs owned by explicit recording bridges are transaction-input storage,
not indexed pages, and may be recycled after the transaction that consumes them
commits. Old immutable indexed snapshots may retain the old physical layout until
their readers release them.

## 15. Stored event pages, fan-in order, and indexed event iteration

Persistent indexed events use whole-page candidate validity with packed event
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

Indexed execution may dynamically commit persistent event payload capacity off the
audio thread. An explicit recording bridge uses slab-backed capture storage supplied
by its non-realtime allocator; the realtime path consumes only already-provisioned
blocks and performs no request-sized indexed allocation.

Event order is semantic and deterministic. Within one producer, events are emitted
in nondecreasing absolute sample-index order. Fan-in uses a stable tie break:

```text
1. absolute sample index
2. stable semantic source/connection ordinal
3. producer-local event order
```

Page evaluation order, worker scheduling, cache/store layout, and request order
must never change equal-timestamp ordering.

Because one logical indexed event read can span multiple stored pages, node-facing
reads expose a segmented ordered range/iterator rather than promise one contiguous
`std::span<TimedEvent>`. Iteration preserves the ordering above and never exposes
events outside input coverage.

## 16. Persistent indexed storage, stable output identity, and `NodeStorage`

Each executable generation still has one canonical fixed-layout `NodeStorage`, but
dynamically sized indexed output data is **not** part of that fixed layout and
should not be owned merely by one JIT generation when the output has stable project
identity.

`NodeStorage` contains storage whose shape is known when the `CompiledGraph` is
built, including:

- sequential `State`;
- optional `IndexedState`, which is non-semantic `tock_coverage()` acceleration
  state only;
- compiler-owned bounded persistent regions;
- bounded reusable workspaces where useful; and
- realtime carry/history/feedback storage selected for persistent placement.

`IndexedState` is not an authoritative indexed-output store and is not shared with
`tick_block()` or propagation callbacks. A node's observable behavior must remain
correct if its `IndexedState` is discarded or independently instantiated.

`GraphExecutor` instead owns stable persistent indexed-output storage plus
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

- stable `tock/persisted` outputs bind new executable generations to their
  existing stable stored entries when semantic compatibility permits;
- `tock/ephemeral` outputs own no persistent output entry to migrate or rebind;
- anonymous tock/persisted outputs receive generation-local stored state;
- tick/persisted finalized data is a stable random-access stored boundary,
  reconciled by its own stable output identity without inventing a tock producer; and
- recording-bridge capture slabs/log records are executor/runtime transaction-input
  storage and are not owned by any published indexed version.

Stable tock/persisted storage reuse is normally **rebinding**, not copying. Page
directories, persistent payloads, file-backed roots, and immutable snapshots may
remain owned by the executor while old/new executable generations refer to
appropriate versions.

The currently specified indexed physical forms are therefore:

```text
persistent authoritative/stored tock/persisted representation
transaction-local paged or prepared transient tock/ephemeral materialization
recording-bridge captured-block log (temporary indexed transaction input)
```

Recording capture storage is append-only by insertion sequence while outstanding,
slab-backed, and independently provisioned from tock progress. Tick/persisted retention adds a semantic lifetime obligation to finalized tick
values and makes the published coverage directly readable by random-access inputs.
It need not adopt tock page layout or acquire a tock callback.

## 17. Indexed transaction workspace

Indexed planning/evaluation needs request-sized temporary storage that is not
necessarily bounded at graph compile time. `GraphExecutor` should own/reuse a
transaction workspace or arena containing things such as:

- per-node/per-port forward-change accumulators;
- per-node/per-port reverse-requirement accumulators;
- computed-output request sets used by the one-call-per-node tock pass;
- forward/reverse region-set work buffers;
- selected stored-page completion plans;
- non-realtime `tock/ephemeral` result/intermediate sample/event values;
- temporary event payloads and segmented views; and
- temporary references to immutable stored snapshots/pages.

The workspace is initialized once per logical indexed batch. All invalidation or
demand roots assigned to that batch contribute into the same accumulators before
the corresponding traversal reaches a node. Transaction-local values remain
shareable across all consumers in the same batch. Once their last consumer is
complete, storage may be reused; future liveness packing is an implementation
optimization.

Tock/ephemeral materialization and all tock callbacks are scheduled off the audio
thread. Temporary pages needed by random-access input connections remain alive
through their consuming evaluation. Recording capture is different: its logical backlog may grow with playback duration and tock
lag, so a non-realtime allocation worker extends slab-backed capture storage while
the realtime path consumes only already-provisioned free blocks.

## 18. Background random-access components and cycle validation

The **random-access evaluation graph** contains tock-produced dependencies,
contextually replayable tick producers and the ordinary sequential input edges
traversed while replaying them. Published persisted outputs are stored boundaries:
the background evaluator reads their selected version and does not traverse their
live producers. Weakly connected evaluation components provide forward/reverse/
evaluation orders, stable endpoint ordinals, coverage accumulators and callback
imports. The existing tock propagation callback ABI is unchanged; replayable
pointwise tick nodes use compiler-synthesized temporal propagation.

Whole-project semantic SCC analysis remains necessary for sequential feedback and
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
no retained compatible counterpart and is introduced into the indexed graph.
Project startup is the case where every project node is newly created. Producing
new machine code for a stable retained node is not by itself semantic creation.

For a new node with computed indexed outputs (`tock/ephemeral` or
`tock/persisted`):

1. ordinary node configuration/resources and optional `IndexedState` are
   initialized;
2. current indexed-input coverages are available once upstream coverage is known;
3. `propagate_forward_coverage()` runs with a node-created/local-change cause and
   may have zero changed indexed-input regions; and
4. exact computed-output coverage is established before demand may target it.

For a newly created computed output, old coverage is empty, so all new coverage is
an exact semantic output change and propagates downstream.

For a replayable tick output, GraphJit derives pointwise coverage from the exact
available coverage of its transitive dependencies; the node does not acquire
authored tock or propagation callbacks. A tick/persisted output acquires
random-access coverage as its finalized values are published, whether or not
its producer is replayable.

A recording bridge's indexed output begins with whatever exact coverage its bridge
semantics establish from captured records/restored retained state. Realtime capture
itself does not publish indexed coverage. When a propagation/tock pass snapshots
new capture records, their `(OutputPortId, GlobalBlockPosition)` identities seed
exact changed/added coverage for the bridge output inside that indexed transaction.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Compatible stored state and
coverage are rebound unless semantic configuration, indexed connection sets,
implementation semantics, sample rate, or another explicit forward cause requires
recomputation.

A `tock/persisted` candidate must become complete over its full new coverage
before publication. A `tock/ephemeral` output needs no persistent
precomputation.

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published indexed semantics enters through an
executor-controlled boundary. Code outside `GraphExecutor` must not mutate active
persistent indexed roots/pages behind the executor's versioning rules.

The architectural invalidation-root set is deliberately closed:

- node-local semantic state/resource mutation;
- finalized persisted tick-data publication/revision;
- a fixed snapshot of newly captured recording-bridge blocks;
- graph semantic configuration/topology/implementation change; and
- project sample-rate change.

Node-type-specific rules determine the exact initial changed regions for a
node-local mutation. Recording captures already identify their originating bridge
output port and global block position, so the executor coalesces the selected
capture records into exact changed coverage per output port. Graph/runtime rules
determine seeds for the other classes. Once seeded, an indexed-input change at a
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
create candidate indexed semantic version
        |
        v
run exact forward coverage/change transaction
        |
        v
complete affected tock/persisted outputs
```

The mutation may have zero changed indexed inputs and still change output coverage
or values. Several application mutations may be applied to one candidate and
seeded together before the batch's forward pass; intermediate semantic versions
need not be externally observable.

An indexed **connection-set change** is another executor-controlled forward cause.
Addition, removal, replacement, or another semantic change to the set of
connections feeding one indexed input conservatively marks that whole logical
input as changed:

```text
changed_input = old_input_coverage | new_input_coverage
```

The node sees the new current input coverage plus that changed region. This may
overinvalidate unaffected fan-in portions but is finite, simple, and correct.

A recording bridge contributes captures differently from an ordinary semantic edit.
Before the indexed pass starts, the executor snapshots the largest currently
available contiguous prefix of capture **insertion sequence** beginning at the
capture log's unprocessed frontier. That snapshot is immutable for the pass.
Captures appended while propagation or tock is running are not added to the current
batch even if they target earlier global positions; they wait for the next pass.

The selected capture records seed ordinary forward invalidation. The bridge's
`tock_coverage()` then materializes required indexed output pages from those
captured payloads as part of the same reverse/tock transaction as downstream work.
Only the final transaction commit publishes a new indexed pages version.

External reads have a correspondingly closed entry-point set: application/UI
random-access fetches and advance background preparation for sequential playback.
The live `CompiledGraph::tick_block()` reads published/prepared data or supplies
its consuming sequential input's neutral value; it never runs tock.
Invalid `tock/persisted` page domains are internal materialization
roots, not a third external caller. The external protocol may combine updates and
fetches in one request; `GraphExecutor` normalizes that shape into mutation seeds,
demand seeds, and one or more legal indexed batches.

## 21. Indexed semantic versions, pages versions, and stale-work rejection

Indexed computation may overlap realtime execution and may be superseded by newer
edits or by captures that arrive after a running batch has taken its snapshot. The
published indexed state uses two version coordinates:

- **semantic version** identifies graph/configuration/resource/sample-rate semantics;
- **pages version** identifies one immutable published indexed page view produced by
  a complete indexed propagation/tock transaction under those semantics.

A recording capture does **not** advance the pages version. Captures accumulate as
pending transaction input. A pages version advances only when the indexed worker
has propagated a fixed set of invalidation roots, completed all required
`tock_coverage()` work, and atomically committed the resulting candidate pages.
A semantic edit may likewise create a new semantic version whose pages are built
before publication.

Indexed transactions therefore operate against a coherent base/target pair:

```text
(semantic_version, pages_version)
```

A reverse/tock transaction uses one immutable base pages version for existing
indexed reads and produces one candidate successor. It never switches its base to
a newer publication in the middle of evaluation. An already-running transaction
may finish against an older immutable base, but stale-work/version validation must
prevent it from being committed as current when its semantic assumptions are no
longer valid.

For `tock/persisted`, a candidate is publishable only when every covered page
domain of every required stored computed output is valid for the target
transaction. Unchanged pages may be structurally shared from the immutable base.

For `tock/ephemeral`, no persistent output validity exists; the selected version
chooses the configuration, coverage, sample rate, and immutable persistent upstream
pages against which the exact request is evaluated.

A recording-capture pass additionally owns one fixed executor-wide capture-sequence
interval, for example `[processed_sequence, snapshot_tail)`. The candidate may read exactly
those captured blocks plus already-published indexed state. Captures with sequence
at or beyond `snapshot_tail` are invisible to that pass and cannot change its
coverage, invalidation roots, reverse requirements, or tock inputs.

If the pass commits, publication atomically advances the pages version and the
capture log's processed-capture frontier together. Capture blocks consumed by that pass
may then be reclaimed because the newly published indexed pages own/materialize
all data they retain. If the pass is cancelled or rejected as stale, its processed
frontier does not advance and its capture blocks remain available for a later pass.

## 22. Direct connection compatibility and explicit recording

The port validator checks **each source channel** against the destination input's
access contract, not equality of the producer callback and consumer callback.
These are connection permissions, not guarantees of audio-thread scheduling:

| Source output | Sequential input | Random-access input |
| --- | --- | --- |
| tick/ephemeral, unreproducible for demanded coverage | allowed | explicit recording policy node required |
| tick/ephemeral, contextually replayable | allowed | allowed by background replay |
| tick/persisted | allowed | allowed over finalized published coverage |
| tock/ephemeral | allowed with prepared data | allowed with temporary page materialization |
| tock/persisted | allowed with prepared data | allowed through retained pages/background recomputation |

An ephemeral sequential output cannot acquire implicit historical storage because
it happens to feed a random-access input; that is the **only special connection
prohibition** introduced by retention. The restriction applies whether the
random-access input is consumed by tick or tock. Tock outputs may feed sequential
inputs, and persisted tick outputs may feed random-access inputs. Tiling does not
change these permissions or silently materialize a new output: a whole-tile
random-access connection requires every selected source channel to satisfy it.
Projections preserve their source channel's original capability.

A tock-produced output directly feeding another node's random-access input must
be materialized into addressable pages before that consumer evaluates. Ephemeral
tock pages may be transaction-local and reclaimed after their readers finish;
persisted pages follow the strict retention guarantee. Replayable tick subgraphs
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

### Sequential playback and page availability

A sequential input consumes its selected pinned published page **as-is**, even if
that page is stale or invalidated for a pending candidate. A genuinely missing page
produces that **input's own `neutral_value`** (including when members of a tile
have different neutral values). Stale does not mean missing: an invalidated old
page remains readable until a replacement is atomically published. The audio thread
never waits for a background transaction, invokes `tock_coverage()`, or allocates
a page on demand. A coherent pinned snapshot prevents concurrent publication from
mutating memory under an audio callback. The neutral fallback does not authorize
out-of-coverage arbitrary random-access reads by node code; those remain invalid.

### Recording bridge capture

A recording bridge consumes realtime data during normal `tick_block()` execution.
As soon as a recording output block is produced—not at the end of the whole root
`tick_block()`—the generated path copies that block into a pre-provisioned capture
block and publishes one immutable capture record:

```cpp
struct CapturedBlock {
    CaptureSequence sequence;
    OutputPortId output_port;
    GlobalBlockPosition position;
    // immutable payload owned by capture storage
};
```

`sequence` is the monotonic insertion order of the executor's recording-capture
log. All participating recording output ports share that ordering domain. It does
not order timeline positions. `output_port` identifies the indexed bridge output
whose value/coverage is changed by the captured payload. Consecutive records may
belong to different output ports, and seeking while playback/recording is active may
append records for positions earlier than records that have not yet been consumed
by indexed execution.

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

### Fixed capture snapshot per indexed pass

At the beginning of one indexed propagation/tock pass, the worker reads the
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
`OutputPortId`. Those output changes are ordinary indexed invalidation roots. From
that point forward there is no bridge-specific downstream scheduler: normal
`propagate_forward_coverage()` determines downstream changed coverage, normal
reverse planning determines the exact input coverage required by implicated nodes,
and normal `tock_coverage()` evaluation materializes bridge output pages and all
affected downstream outputs.

A bridge's captured payload is transaction-input storage, not a published indexed
view. Indexed callbacks never observe captures outside the fixed snapshot, and
capture insertion never changes a running transaction's inputs.

## 23. Complete stored outputs and physical backing

A tock/persisted output is published only after its entire exact coverage has
been materialized for the candidate semantic version. Candidate versions may have
invalid pages while rebuilding, but partial candidate validity is never exposed as
the published stored output.

Complete materialization does not require all bytes to remain in anonymous RAM.
Physical backing may use mmap/files, compression, deduplicated immutable pages,
or copy-on-write roots, but **never automatic eviction** of generated persisted
coverage. Invalidation retains readable old published pages while a successor is
computed. Pages may be removed from the logical retained output only when its
coverage no longer includes them. Superseded immutable physical versions may be
reclaimed after their readers release them. Any representation accessed directly
by realtime execution must meet realtime access requirements.

Tick/persisted output differs in how values become final: `tick_block()` may
revise positions under normal history/latency semantics, and finalized regions
then become retained, randomly readable published data. Persistence does not impose
whole-block-or-none authoring semantics or require a tock-produced bridge. Its
retained physical representation need not equal the tock page-store design.

## 24. Recording capture allocation, snapshots, and reclamation

The recording bridge uses three independently progressing runtime roles so arbitrary
tock latency does not have to fit inside a bounded realtime staging window. Capture
is active only while playback/recording is active; when playback stops, no new
records are appended and the indexed worker can drain the remaining backlog.

```text
non-realtime allocation worker
        |
        | allocate/recycle slabs
        v
pre-provisioned free capture blocks
        |
        | realtime thread is the consumer
        v
recording output production during tick_block()
        |
        | memcpy + metadata + publish
        v
append-only capture log
        |
        | fixed sequence snapshot per pass
        v
indexed propagation / reverse planning / tock worker
        |
        | atomic indexed commit
        v
published pages version + reclaimed capture blocks
```

### 24.1 Realtime capture

The realtime path does not wait for propagation/tock and does not scan the graph at
the end of `tick_block()`. At the precise production point of a recording output
block it:

1. consumes one already-provisioned free capture block;
2. copies the produced sample/event payload into it;
3. writes the output-port ID, global block position, and next capture sequence; and
4. publishes the immutable record to the capture log.

Only the realtime path consumes blocks from the free-capacity pool. The allocator
never takes a free block back from underneath it, so realtime priority is structural
rather than a lock/scheduling race.

### 24.2 Slab provisioning

A dedicated non-realtime allocator keeps free capture capacity near a configured
target for the shared recording-capture log. It allocates append-only slabs in units
large enough to amortize allocation
cost but small enough to provision promptly, splits/reuses them as capture blocks,
and publishes those free blocks for realtime consumption.

Provisioning is independent of indexed execution. If a propagation/tock DAG takes
four seconds, forty seconds, or longer, captured blocks may accumulate in ordinary
dynamically allocated recording storage while the allocator continues extending
capacity. Slow tock execution therefore does not by itself force block dropping or
a fixed handoff overrun.

No finite reserve can provide a mathematical no-allocation/no-block/no-drop
guarantee if the allocation worker itself is prevented from running indefinitely
or the process exhausts addressable/storage resources. That catastrophic policy is
separate from normal bridge semantics; the architecture must not impose ordinary
block loss merely because indexed execution is slower than realtime.

### 24.3 Indexed consumption

The indexed worker never races the capture tail during a pass. It snapshots the
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
6. publishes one coherent successor pages version for the complete transaction.

This is the normal indexed execution routine. There is no intermediate
"capture-pages publication" that downstream work waits for.

### 24.4 Reclamation

Published indexed versions never reference capture storage. Bridge tock execution
copies/materializes everything retained by the candidate into version-owned indexed
pages. Therefore, after successful atomic commit, every captured block in the
committed sequence interval can be returned immediately to the allocator/reuse
pool.

Old indexed readers pin old indexed pages, not raw capture blocks. Conversely, a
failed/cancelled/stale transaction cannot reclaim its capture interval as consumed;
those records remain pending until some later transaction commits them.

## 25. Indexed event/sample reads from node callbacks

Random-access sample reads address global covered sample indices. Persisted tick
and tock outputs provide published direct/page-backed access; tock/ephemeral
outputs feeding a random-access input are materialized into transaction-local
addressable pages. Replayable tick subgraphs may produce temporary results when
their consumers require materialization.

Indexed event reads may span several stored segments/pages and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Random-access accessors expose exact input coverage. Debug validation catches
requests outside coverage. A `tock_coverage()` accessor reads random-access
inputs from the selected immutable transaction view, which may include finalized
persisted tick data, background-replayed tick values, tock pages, and an explicit
recorder's published output. It never reads mutable live tick storage or captures
appended after the transaction snapshot cutoff.

## 26. External indexed-access result lifetime

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

Every externally returned indexed result is associated with the
`(semantic_version, pages_version)` pair from which it was read.

For `tock/persisted`, a newer candidate is not externally presented as a partially
materialized stored output. Until the entire required stored candidate is complete,
the newer version is pending/not-ready and callers may continue using an older
completed published result. Missing candidate pages must never be represented as
neutral samples, zero events, or artificial missing coverage.

For `tock/ephemeral`, an external request may evaluate exact requested coverage
against a selected immutable version pair and return that pair with the result. The
host API may satisfy the request synchronously or expose explicit pending/future
behavior; the exact ABI is implementation work.

A caller must not combine independently returned data from different semantic or
pages versions as if it were one coherent indexed result.

## 27. Outward change notification

After an indexed candidate becomes published—including a transaction that
consumed recording-bridge captures—the executor may emit a coalesced indexed-output
change notification for subscribed external consumers.

Conceptually it contains:

```text
output identity
indexed semantic version/revision
published pages version
new coverage (or a coverage-changed indication)
exact changed IndexedCoverage
```

Notification is emitted from the executor publication/change boundary, not
recursively from individual callbacks. Presentation/UI code can react by issuing
ordinary indexed access requests. The notification does not itself force
`tock/ephemeral` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and semantic invalidation are separate events. Producing a new
`CompiledGraph` generation does not by itself make stable tock/persisted output
stale or revoke the retention guarantee of tick/persisted output.

`GraphExecutor` reconciles old/new generations using stable concrete-node/output
identity. Compatible persisted outputs rebind to the existing stable store rather
than copying payloads merely because machine code changed.

Important cases:

- tock/persisted with unchanged semantics preserves coverage and payloads;
- tock/persisted with changed semantics runs exact invalidation and rebuilds
  invalid candidate pages before publication;
- tick/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no semantic persistent payload to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic endpoint identity rather than builder
handles, lowered node indices, or ORC-generation-local ordinals.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output realtime root. It does
not gain synthetic indexed output ports or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal indexed endpoints to
generated random-access component executors, persisted bindings,
advance-preparation plans,
and explicit recording-bridge capture/output identities. Static indexed topology is
specialized during lowering rather than rediscovered for every request or block.

### Reuse whole-graph analysis products during lowering

Whole-project lowering reuses one SCC decomposition of the complete semantic
dependency graph to reject random-access input edges within a semantic SCC.
Separately, expand the background evaluation graph through sequential input edges
of replayable tick nodes. Reject any unresolved replay dependency cycle; a valid
published persisted output is a terminal stored boundary for its covered requests.
Checking only labeled random-access edges is **not** sufficient for this second
check. Reuse the same stable ordinals and graph-analysis data; do not perform a
new reachability search per output.

SCC IDs, member ranges, condensation-DAG/topological order, dense node ordinals,
and reusable adjacency storage should be carried forward where useful. Indexed
component planning, live scheduling, liveness/storage planning, access/retention
lowering, and later batching/fusion/vectorization should consume those facts
instead of rebuilding equivalent graph views.

This is an efficiency rule, not a semantic requirement to collapse distinct graph
relations into one. Explicit-DAG validation, detach legality, same-slice realtime
dependencies, and complete semantic dependency relations answer different
questions. Detach validation may still require pre-feedback reachability. Share
ordinals/storage/traversal scratch where the edge relation actually matches.

The initial compiler should recompute SCCs from scratch for each graph compilation
rather than implement dynamic SCC maintenance. A complete Tarjan/Kosaraju-style
pass is linear, simple, deterministic, and expected to be negligible beside LLVM
lowering/optimization until profiling proves otherwise.

`GraphExecutor` owns:

- canonical fixed-layout `NodeStorage` for each retained executable generation;
- stable persistent indexed stores/immutable roots for `tock/persisted`
  outputs, plus per-generation bindings;
- indexed semantic versions plus immutable pages versions and candidate/published
  snapshots;
- the recording-capture log, its processed-sequence frontier, slab/block reclamation
  state, and coordination with a non-realtime allocation worker;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/tock transactions;
- completion scheduling for candidate `tock/persisted` outputs; and
- external indexed access/change-notification lifetime.

## 30. Deliberately open implementation/tuning choices

Implementation choices include physical page backing, snapshot directories,
transaction-workspace reuse, capture slab sizes and queue/watermark strategy,
worker scheduling, cancellation granularity, and post-correctness SIMD/fusion/
value-specialization cost models. None of these may weaken the authored persisted
retention obligation or introduce audio-thread tock execution.

The following are **not** implementation choices: static constexpr concrete node
port schemas; independent input access/output production/output retention;
explicit opt-in replayability with contextual dependency validation; no implicit
recording of unreproducible ephemeral tick sources; background-only tock and
propagation; stale-page-as-is/missing-page-per-input-neutral playback; no persisted
page eviction except loss of covered positions; exact coverage and forward changes;
value-blind reverse demand; fixed capture-prefix transactions; and no unresolved
random-access evaluation cycles.

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
intervenes, and rejects unresolved background evaluation cycles. A tock ->
random-access consumer requires page materialization. Capture bridges must have
valid sequential-input/tock-output shapes and realtime-safe pre-provisioned capture
capacity. Unsupported event-replay contracts must be rejected, not silently treated
as sample replay. Errors identify the specific source channel, input, dependency
path, or offending node where practical.

## 32. Implementation landing order

This is the **normative dependency order** for the next documentation/implementation
migration; [graph_jit_direction.md](./graph_jit_direction.md) and the application
architecture should reference this list rather than propose a divergent one.
The target API/retention/replay semantics described here are **not landed** merely
because this design is documented. The supplied implementation still contains
`IndexedProducer` and legacy recorder staging.

1. **Delete legacy execution and dynamic concrete-port declarations.** Stop
   constructing `GraphLowerer`/`GraphCompiler`/`RuntimeGraphRoot` during module
   loading; derive source introspection from `ConfiguredGraph`. Remove the legacy
   generated routing nodes, type-erased executor/facade fallbacks and their old
   runtime-root ABI. Enforce static constexpr port schemas in public and internal
   concrete-node construction; retain dynamic graph topology and static-schema
   graph-specific port metadata.
2. **Migrate the independent port schema and connection planner.** Carry input
   access, output production and output retention through reflection, serialized
   configured graphs, compiler records and per-channel tiling. Delete inferred
   input/output access conversion helpers and equality-based compatibility. Enforce
   the narrow explicit-recorder boundary, strict persisted retention and
   background-only tock rule. Preserve existing sequential history/latency.
3. **Add the replayability trait and dependency planning.** Validate the existing
   `tick()`-only static node shape and semantic promise, reflect the trait, derive
   contextual replayability, synthesize pointwise forward/reverse coverage and
   include eligible imported generated `tick_block()` wrappers in the background
   DAG. Retain the current tock propagation callback API.
4. **Implement ordinary background indexed execution before recording consumption.**
   Finish the reusable batch frame/reflected callback ABI; import forward/reverse/
   tock callbacks and emit generated F/R/evaluation programs. Implement
   `GraphExecutor`, temporary tock paging, complete persisted candidate pages,
   pinned snapshot reads (stale-as-is, missing-per-input-neutral), atomic publication
   and advance preparation. Verify that no audio-thread path can invoke tock.
5. **Make explicit recording operational on that executor.** Define bridge node
   semantics/capture ABI; implement independent non-realtime slab provisioning and
   realtime-safe block capture at production time. Consume fixed capture-sequence
   snapshots through the already working F/R/evaluation transaction; publish once
   and reclaim committed capture blocks without losing cancelled/stale work.
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
4. An unreproducible tick/ephemeral source cannot directly satisfy random-access
   demand. Persisted tick, reproducible tick and either tock output can; a tock
   output feeding a random-access input is page-materialized even when ephemeral.
5. Tiling preserves per-channel contracts and creates neither an implicit recording
   policy nor an implicit producer.
6. `tock_coverage()` and authored propagation callbacks run only off the audio
   thread. A sequential input plays a stale published page as-is and substitutes
   **its own** `neutral_value` for a missing page without waiting.
7. Persisted outputs retain all generated pages in coverage without automatic
   eviction. Coverage removal can remove logical pages; superseded physical versions
   are reclaimed only after readers release them. Memory growth is author-selected.
8. Exact `IndexedCoverage` and forward changes are independent of the physical
   page grid. Reverse demand is value-blind; replayable pointwise tick propagation
   is synthesized from static dependencies.
9. The background demand/replay graph has no unresolved cycles; sequential feedback
   retains its separate scheduling semantics.
10. Recording nodes use explicit authored policies, pre-provisioned capture slabs,
    monotonic capture *insertion* sequences and fixed-prefix transactions. Capture
    insertion is not page publication; committed outputs never reference capture
    storage and commit alone advances the processed frontier.
11. `IndexedState` is non-semantic tock-only acceleration; persistent output stores
    and capture backlogs are executor-owned sidecars, distinct from fixed `NodeStorage`.
12. The legacy generated-node graph executor and dynamic concrete-port fallbacks
    are deletion targets; the package/configuration JIT and GraphJit's imported
    concrete-node LLVM remain.

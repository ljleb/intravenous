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

> `IndexedOutputConfig` means globally addressed, order-independent production by
> `tock_coverage()`. Indexed outputs publish exact finite **coverage**. Outside
> coverage, the output does not exist for indexed dependency purposes and node code
> may not request it.

> Output production and retention are independent. `RealtimeOutputConfig` is
> produced by `tick_block()` with its history/latency contract;
> `IndexedOutputConfig` is produced by `tock_coverage()`. Either may be
> `ephemeral` or `persisted`. Retention controls whether finalized values must be
> kept, not which callback produces them.

> `IndexedInputConfig` consumes indexed outputs. `RealtimeInputConfig` consumes
> realtime outputs. GraphJit does not implicitly bridge these execution domains.
> Realtime-to-indexed transfer is represented by explicit bridge nodes whose
> realtime side captures data and whose indexed output participates in the ordinary
> indexed invalidation/tock machinery.

> Forward propagation records exact semantic coverage/change without forcing
> evaluation. Computed indexed outputs require an exact forward-coverage provider.
> Reverse propagation is value-blind and may conservatively over-request. Stored
> candidate pages are a recomputation granularity only; page boundaries never widen
> semantic coverage or forward changed regions.

> Indexed invalidation and demand are processed as **batched transactions**. All
> roots for one batch are accumulated before traversal. Fan-in/fan-out coverage is
> unioned in transaction workspace so each implicated node runs
> `propagate_forward_coverage()`, `propagate_reverse_coverage()`, and
> `tock_coverage()` at most once each for that batch. The current callbacks remain
> one-node callbacks; transaction batching does not imply a multi-node callback ABI.

> `IndexedState` is non-semantic acceleration state visible only to
> `tock_coverage()`. It may affect execution pace but may not affect values,
> coverage, dependency requirements, or any observable result.

> No indexed dependency may participate in a directed cycle. After whole-project
> semantic SCC detection, every indexed connection must strictly leave its source
> node's semantic SCC. Realtime SCCs may export indexed data outward, but indexed
> edges never participate in cyclic execution semantics.

> Recording bridges capture realtime-produced blocks into immutable transaction-input
> storage as soon as the recording output block is produced. Capture records are
> globally ordered by insertion sequence but carry their own output-port identity and
> global block position, so seeking may append changes to earlier positions. Each
> indexed pass snapshots a fixed capture-sequence prefix; later captures wait for the
> next pass. The selected captures seed ordinary exact invalidation, reverse planning,
> tock execution, and one atomic indexed pages-version publication.

This is an incremental indexed-evaluation model integrated with realtime graph
execution, not a second realtime scheduler and not a storage class.

## 1. Port model

Payload kind and access model are orthogonal. Ordinary DSP nodes therefore have
four declaration combinations:

| Port kind | Realtime access | Indexed access |
| --- | --- | --- |
| sample | bounded sequential/current-block timing | arbitrary global sample positions inside coverage |
| event | bounded sequential/current-block timing | arbitrary global event regions inside coverage |

A realtime declaration carries its finite history/latency timing contract. An
indexed declaration carries no realtime history/latency contract.

`tick_block()` is sequential realtime execution. `tock_coverage()` is
order-independent indexed evaluation. Merely advancing realtime time is not an
indexed invalidation event.

## 2. `IndexedCoverage` is the only indexed domain boundary

Every indexed output publishes a finite **coverage**: a canonical union of
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

An indexed input exposes the canonical union of the mapped coverages of the
connected indexed outputs. Both `tick_block()` and `tock_coverage()` can
inspect this input coverage so node algorithms can iterate only meaningful
regions rather than traversing uncovered timeline gaps.

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

For a published `indexed/persisted` output, every page with a nonempty page domain is
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

A `indexed/ephemeral` output owns no persistent materialization. A sparse request is
intersected with exact coverage and stays exact through that output. Its
`tock_coverage()` work may therefore compute only the requested covered regions,
writing directly into caller/result or transaction-local storage.

A published `indexed/persisted` output is different: the whole output coverage has
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
candidate is valid, the `indexed/persisted` output is complete for its target
`(semantic_version, pages_version)` pair and may become publishable.

A realtime/persisted output has a retention obligation for finalized realtime
values, but its values are produced by `tick_block()` rather than regenerated by
`tock_coverage()`. Persistence does not make that output an indexed source and does
not create an
implicit page-publication path. Realtime-to-indexed transfer requires an explicit
bridge node. The bridge's indexed output is materialized by ordinary indexed
`tock_coverage()` execution from immutable captured blocks and is published together
with the rest of the indexed transaction.

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
intersects a `indexed/persisted` candidate page, the whole page becomes invalid locally,
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

A `indexed/ephemeral` request has no such page promotion: its exact covered demand is
reverse-propagated directly.

This is the deliberate asymmetry:

- forward semantic change is never widened merely because a stored page becomes
  invalid;
- candidate completion for `indexed/persisted` may page-promote work because the stored
  version must become complete; and
- demand through `indexed/ephemeral` remains exact because no retained output page is
  being completed.

## 7. Forward changes are independent of demand

Forward propagation is independent of indexed access and is not limited to
incoming indexed-edge changes.

The executor has a closed set of **external invalidation roots**. Every persistent
indexed semantic invalidation originates from one of these classes:

1. **node-local semantic mutation**: an application/UI operation changes node
   state, configuration, or a resource according to that node type's own semantic
   rules;
2. **recording-bridge capture snapshot**: a fixed prefix of newly captured blocks changes one or more explicit bridge indexed outputs at their recorded global positions;
3. **graph semantic configuration change**: node creation/removal/replacement,
   indexed connection-set changes, or another graph/configuration change that
   changes indexed dependencies or implementation semantics; or
4. **project sample-rate change**: computed indexed semantics are reevaluated
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
For `indexed/persisted`, every candidate page intersecting `changed`, an added/removed
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
regions and multiple changed inputs in that one callback invocation.

For every node that declares at least one `indexed/ephemeral` or `indexed/persisted` indexed
output, an explicit forward-coverage implementation (or a future equivalent
multi-node/batched implementation) is **mandatory**. Coverage is semantic and exact;
there is no generally correct fallback that can invent it. A framework may still
synthesize trivial glue when exact coverage is mechanically declared by another
static facility, but it must not silently preserve old/empty coverage or substitute
a conservative superset.

A node whose outputs are exclusively realtime-produced has no indexed forward
callback obligation. An explicit realtime-to-indexed bridge is different: its
indexed output is an ordinary computed indexed output, so it must provide exact
forward-coverage semantics (or an equivalent framework-defined exact rule) and
ordinary `tock_coverage()` materialization for that output.

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

For `indexed/ephemeral`, the output requirement is the exact covered demand. For
`indexed/persisted`, candidate completion first selects the complete covered domains of
invalid stored pages, and those selected domains become the requirements supplied
to reverse planning.

Persistent indexed data is a reverse-propagation boundary only when it is valid
for the `(semantic_version, pages_version)` pair selected by the batch:

- a valid `indexed/persisted` page/domain for the selected target/base version pair
  satisfies that requirement and stops reverse propagation through that region;
- an invalid or nonexistent `indexed/persisted` candidate page does **not** stop reverse
  propagation merely because an older physical page still exists. Its complete
  covered page domain becomes a materialization requirement and reverse planning
  continues through its producer; and
- `indexed/ephemeral` owns no persistent result and therefore never forms a persistent
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

For a `indexed/ephemeral` output that participates in a live pull path, any reverse
mapping that must execute dynamically on the audio thread is part of the
realtime-safety contract. GraphJit should specialize/precompute such mappings
wherever possible.

`IndexedState` is **not** visible here. Dependency requirements may not depend on
memoization history.

## 10. `tock_coverage()`

`tock_coverage()` is the one-node indexed evaluation callback for outputs whose
output access is `IndexedOutputConfig`. Its context carries requested
`IndexedCoverage` per computed indexed output; one invocation may therefore
compute many disjoint regions and several outputs of the same node.

Within one indexed batch, reverse planning first finishes accumulating the final
requested coverage for every implicated output. Forward evaluation then calls
`tock_coverage()` **at most once per implicated node for the whole batch**. Requests
for several outputs, several disjoint regions, and several downstream consumers
are therefore coalesced before the callback runs.

The callback receives only work that needs computation for the target operation:

- a `indexed/ephemeral` output receives exact demanded covered regions; and
- a `indexed/persisted` output receives selected invalid candidate page domains, each
  already intersected with exact output coverage.

It never receives uncovered portions of a physical page.

The callback:

- sees indexed inputs and computed indexed outputs;
- sees the project sample rate for the semantic version being evaluated;
- operates on covered global sample/event regions;
- cannot request indexed inputs outside their coverage;
- cannot depend on realtime-only inputs or outputs;
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
makes that safe. If an indexed/ephemeral path is selected for audio-thread execution, it may never
wait on a lock held by background/UI tock work merely to share memoization;
GraphExecutor can instead reserve a live instance, duplicate state, or otherwise
provide exclusive realtime-safe access. Ephemeral retention alone does not imply
that every tock implementation is realtime-safe.

Successful tock completion is transactional. For samples, every requested covered
sample/channel of every requested output is completely initialized. For events,
the callback emits the complete event sequence for every requested covered region;
zero events is a complete result. Failed, cancelled, or superseded work commits no
partial output as valid.

Within one output invocation, events are emitted in nondecreasing global timestamp
order. Equal-timestamp producer-local order is preserved.

A single callback remains preferable to mandatory per-output callbacks because a
node may share useful work among several outputs. Realtime eligibility is a
callback/execution capability, not a consequence of `ephemeral` retention. If
GraphJit chooses to execute an indexed/ephemeral path on the audio thread, the
selected callback path must satisfy the usual bounded realtime contract; otherwise
that result must be prepared outside the realtime thread or not used as a live
pull.

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
view, one coherent set of indexed/persisted bindings, and—when a recording bridge
has pending captures—one fixed capture-sequence snapshot selected before forward
propagation begins. Captures published after that cutoff are not part of the batch.
Pure stored reads may need neither R nor T; a pure invalidation
batch may defer R/T; and a mutation-plus-fetch operation may run F then R/T before
returning results. The batching invariant constrains how work is coalesced when a
phase is present, not which phases every caller must execute.

There are two closed classes of **external demand roots**:

1. **application/UI indexed fetches**, such as JSON-RPC requests for one or more
   indexed outputs/regions; and
2. **live root execution demand** originating from `CompiledGraph::tick_block()`
   when realtime production needs indexed values for the current root invocation.

The host protocol need not expose these as one-request/one-batch operations. One
application request may contain multiple fetches, and an update request may also
ask for results after the update. The executor normalizes the request into batched
mutation and demand roots.

There is also one important **internal materialization root**: every invalid
`indexed/persisted` candidate page domain that must be completed before the target
version pair can publish. Internal page-completion roots use the same reverse
and tock machinery as explicit reads.

### Demand-driven access

For requested `indexed/ephemeral` sink outputs, the batch:

1. intersects every explicit request with exact output coverage;
2. unions all covered requirements for each output;
3. reverse-propagates the coalesced requirements through `indexed/ephemeral` producers
   until reaching valid persistent boundaries or source nodes;
4. evaluates implicated nodes in forward dependency order, at most once per node;
   and
5. returns/forwards the requested materialization from caller, transaction, direct
   consumer, or bounded live-transient storage.

A request for a published `indexed/persisted` output does not trigger partial
reconstruction of that output. It reads the requested subset directly from the
complete persistent representation selected for the batch. Realtime/persisted
outputs are not indexed request targets merely because they are retained; crossing
that execution-domain boundary requires an explicit indexed bridge output.

### `indexed/persisted` candidate completion

Forward invalidation may create a candidate version with invalid stored page
domains. Before reverse planning, the executor promotes every invalid stored page
to its complete covered page domain:

```text
materialization_requirement(page) = page_interval & candidate_output.coverage
```

Those complete page domains are then unioned with all other materialization demands
for the same indexed batch. To make the candidate publishable, the executor:

1. gathers all invalid/nonexistent `indexed/persisted` page domains that the candidate
   requires;
2. unions those internal roots with any explicit demand roots allowed in the same
   transaction;
3. runs one reverse-order pass, coalescing downstream requirements before each
   node and stopping per-region at persistent data valid for the selected semantic
   version;
4. runs one forward-order tock pass, evaluating every implicated node at most once
   for its complete consolidated output requirements; and
5. commits computed stored pages transactionally. A semantic candidate may publish
   only after every covered `indexed/persisted` page/domain it owns is valid.

Conceptually:

```text
all invalidation roots for batch
        |
        v
one exact forward pass (F)
        |
        v
exact changed regions + invalid indexed/persisted pages
        |
        v
promote invalid pages to complete covered page domains
        |
        +-------------------------------+
        |                               |
        |                    explicit UI/live demand roots
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
one forward evaluation pass (T)
  each implicated node <= 1 tock callback
                        |
                        v
complete candidate stored pages / transient results
                        |
                        v
publish coherent candidate when all required stored state is complete
```

A valid indexed/persisted page may satisfy an upstream requirement without further
traversal. An invalid candidate stored page cannot: its older retained payload is
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
                +--> invalidate intersecting indexed/persisted candidate pages locally
                |
                `--> union exact semantic changes into downstream accumulators
```

Fan-in never causes repeated callbacks within the batch. Every upstream path that
can reach a node in forward order has contributed to that node's per-input change
accumulator before its callback runs. Fan-out distributes the callback's consolidated
output changes into downstream accumulators.

Forward propagation continues **through** `indexed/persisted` computed outputs. A stored
output is not an invalidation cut: keeping an older page payload alive does not
make downstream semantics current for a new candidate version. Page boundaries
only decide which local candidate pages become invalid; the exact semantic changed
regions continue downstream without widening.

The forward phase itself does not recursively evaluate nodes. After it finishes,
all invalid stored page domains are known and can be promoted to complete covered
page-domain demand roots for the batch's R/T phase. The executor may defer that
materialization, but a candidate containing `indexed/persisted` outputs cannot become
published for its target version pair until all of its covered stored page domains
are valid.

## 13. Output access and retention

Output access and retention are independent configuration properties:

```cpp
struct RealtimeOutputConfig {
    std::size_t history = 0;
    std::size_t latency = 0;
};

struct IndexedOutputConfig {};

enum class OutputRetention {
    ephemeral,
    persisted,
};
```

The parent `OutputConfig` carries one access variant plus one
`OutputRetention`. The four meaningful combinations are:

| output access | retention | semantics |
| --- | --- | --- |
| realtime | ephemeral | sequential `tick_block()` output with no retention requirement |
| realtime | persisted | sequential `tick_block()` output whose finalized values are retained |
| indexed | ephemeral | order-independent `tock_coverage()` output with no retention requirement |
| indexed | persisted | order-independent `tock_coverage()` output retained over complete exact coverage |

Retention never changes the producer callback. In particular, a
realtime/persisted output keeps ordinary realtime history/latency semantics and
may revise any position still writable under that contract. Only finalized
positions are subject to the persisted retention guarantee.

`ephemeral` means that retaining generated values is not semantically required.
It does not prohibit temporary compiler/executor caching or page materialization.
`persisted` is a runtime retention guarantee; external file/project serialization
is a separate concern. Realtime/persisted retention does not imply indexed access.
If realtime-produced data must enter indexed evaluation, an explicit bridge node
captures it and exposes a separate indexed output as described in sections 22 and
24.

Indexed/ephemeral computation may be suitable for live execution, but realtime
safety is an execution/callback capability rather than a consequence of retention.

## 14. Stored pages share the canonical whole-graph block quantum

Persistent output storage uses canonically aligned fixed-width pages whose width
matches the fixed whole-graph root block size for the active layout generation.
This is a physical representation choice; semantic coverage and realtime
history/latency remain independent of page boundaries.

For an indexed/persisted output:

```text
page_interval(i) = [i * B, (i + 1) * B)
page_domain(i)   = page_interval(i) & output.coverage
```

A candidate page becomes valid only after its complete nonempty `page_domain` has
been materialized for the target `(semantic_version, pages_version)` pair. One
`tock_coverage()` call may
cover many selected pages; page boundaries do not imply one callback per page.

Realtime/persisted retention is not governed by this indexed page quantum merely
because the values are retained. `tick_block()` may author/revise positions
according to its normal history/latency contract, and only finalized positions are
subject to that retention guarantee. If those values must become indexed, an
explicit bridge exposes an indexed output whose materialized pages then follow the
ordinary indexed page rules above.

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
    OutputAccessConfig access;
    OutputRetention retention = OutputRetention::persisted;
    // Versioned coverage/page/root state and stable payload storage.
};
```

Ownership rules:

- stable `indexed/persisted` outputs bind new executable generations to their
  existing stable stored entries when semantic compatibility permits;
- `indexed/ephemeral` outputs own no persistent output entry to migrate or rebind;
- anonymous indexed/persisted outputs receive generation-local stored state;
- realtime/persisted retention is reconciled according to the realtime output's
  stable identity, but it is not an indexed page binding; and
- recording-bridge capture slabs/log records are executor/runtime transaction-input
  storage and are not owned by any published indexed version.

Stable indexed/persisted storage reuse is normally **rebinding**, not copying. Page
directories, persistent payloads, file-backed roots, and immutable snapshots may
remain owned by the executor while old/new executable generations refer to
appropriate versions.

The currently specified indexed physical forms are therefore:

```text
persistent authoritative/stored indexed/persisted representation
transaction/live transient indexed/ephemeral materialization
recording-bridge captured-block log (temporary indexed transaction input)
```

Recording capture storage is append-only by insertion sequence while outstanding,
slab-backed, and independently provisioned from tock progress. Realtime/persisted
retention adds a semantic lifetime requirement to finalized realtime values; it
does not, by itself, select an indexed physical form or transfer protocol.

## 17. Indexed transaction workspace

Indexed planning/evaluation needs request-sized temporary storage that is not
necessarily bounded at graph compile time. `GraphExecutor` should own/reuse a
transaction workspace or arena containing things such as:

- per-node/per-port forward-change accumulators;
- per-node/per-port reverse-requirement accumulators;
- computed-output request sets used by the one-call-per-node tock pass;
- forward/reverse region-set work buffers;
- selected stored-page completion plans;
- non-realtime `indexed/ephemeral` result/intermediate sample/event values;
- temporary event payloads and segmented views; and
- temporary references to immutable stored snapshots/pages.

The workspace is initialized once per logical indexed batch. All invalidation or
demand roots assigned to that batch contribute into the same accumulators before
the corresponding traversal reaches a node. Transaction-local values remain
shareable across all consumers in the same batch. Once their last consumer is
complete, storage may be reused; future liveness packing is an implementation
optimization.

Bounded live `indexed/ephemeral` materialization is planned by GraphJit and must
not depend on request-sized dynamic allocation from the audio thread. Recording
capture is different: its logical backlog may grow with playback duration and tock
lag, so a non-realtime allocation worker extends slab-backed capture storage while
the realtime path consumes only already-provisioned free blocks.

## 18. Indexed connected components and the no-indexed-cycle rule

The **indexed subgraph** is the project graph restricted to indexed edges and their
participating nodes.

An **indexed connected component** is a maximal weakly connected component of that
indexed subgraph. It is a planning partition, not an SCC. Lowering may precompute
per component:

- requestable indexed output ordinals;
- reverse dependency order;
- forward change/evaluation order;
- fanout/convergence structure;
- constant node/port/storage offsets and callback targets;
- trivial/no-op propagation operations; and
- bounded reusable planning workspace where useful.

Feedback validation uses a separate **whole-project semantic dependency graph**.
Its vertices are concrete nodes. Its directed edges represent logical data
dependencies of every access direction that can carry causality between nodes,
including realtime and indexed connections. Detached/feedback connections are
restored for semantic cycle membership even though detach removes their same-slice
tick dependency.

The final indexed constraint is intentionally simple:

> **No indexed dependency edge may participate in a directed cycle.**

Equivalently, after computing SCCs of the complete semantic dependency graph,
every indexed connection must have source and target nodes in different semantic
SCCs. An indexed self-loop is invalid.

A node may itself participate in a realtime SCC and export indexed data outward:

```text
        realtime feedback
      +-------------------+
      |                   |
      v                   |
     A -----------------> B
     |
     `---- indexed -----> C
```

This is legal when `C` is outside the realtime SCC. What is forbidden is any
indexed edge whose consumer remains in the same semantic SCC.

This rule applies to every indexed dependency regardless of whether its source node
also participates in realtime execution or owns realtime/persisted outputs. Indexed
access therefore never introduces a second fixed-point/feedback model and never
participates in the cyclic sequential execution semantics used for realtime
feedback. Realtime SCC semantics remain the sole mechanism that legalizes graph
cycles.

GraphJit performs this check after the complete project graph and explicit
feedback semantics are known. Checking only same-slice realtime scheduling SCCs is
insufficient because an indexed edge may itself close a mixed semantic cycle.

## 19. Node creation and coverage publication

Having `tock_coverage()` does not imply that a node evaluates merely because a
project or executable generation was created.

The primitive lifecycle event is **semantic concrete-node creation**: a node has
no retained compatible counterpart and is introduced into the indexed graph.
Project startup is the case where every project node is newly created. Producing
new machine code for a stable retained node is not by itself semantic creation.

For a new node with computed indexed outputs (`indexed/ephemeral` or
`indexed/persisted`):

1. ordinary node configuration/resources and optional `IndexedState` are
   initialized;
2. current indexed-input coverages are available once upstream coverage is known;
3. `propagate_forward_coverage()` runs with a node-created/local-change cause and
   may have zero changed indexed-input regions; and
4. exact computed-output coverage is established before demand may target it.

For a newly created computed output, old coverage is empty, so all new coverage is
an exact semantic output change and propagates downstream.

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

An `indexed/persisted` candidate must become complete over its full new coverage
before publication. An `indexed/ephemeral` output needs no persistent
precomputation.

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published indexed semantics enters through an
executor-controlled boundary. Code outside `GraphExecutor` must not mutate active
persistent indexed roots/pages behind the executor's versioning rules.

The architectural invalidation-root set is deliberately closed:

- node-local semantic state/resource mutation;
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
complete affected indexed/persisted outputs
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
indexed fetches and indexed reads required by a live `CompiledGraph::tick_block()`
invocation. Invalid `indexed/persisted` page domains are internal materialization
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

For `indexed/persisted`, a candidate is publishable only when every covered page
domain of every required stored computed output is valid for the target
transaction. Unchanged pages may be structurally shared from the immutable base.

For `indexed/ephemeral`, no persistent output validity exists; the selected version
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

## 22. Indexed reads and explicit realtime-to-indexed bridges

Direct port compatibility does not cross execution domains:

```text
RealtimeOutputConfig -> RealtimeInputConfig
IndexedOutputConfig  -> IndexedInputConfig
```

An `IndexedInputConfig` therefore never reads mutable realtime output storage and
GraphJit never silently inserts a generic realtime-to-indexed page/ring adapter.
When a product feature needs to carry realtime-produced material into the indexed
world, it uses an explicit bridge node with realtime-side capture semantics and an
ordinary indexed output.

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

An indexed/persisted output is published only after its entire exact coverage has
been materialized for the candidate semantic version. Candidate versions may have
invalid pages while rebuilding, but partial candidate validity is never exposed as
the published stored output.

Complete materialization does not require all bytes to remain in anonymous RAM.
Future physical optimizations may use mmap/file backing, compression, deduplicated
immutable pages, copy-on-write roots, or other stable representations. Any
representation used directly by realtime execution must still satisfy realtime-
access requirements.

Realtime/persisted output differs only in how values become final: `tick_block()`
may revise positions according to normal history/latency semantics, and finalized
regions then become subject to the retention guarantee. Persistence does not impose
whole-block-or-none authoring semantics. The retained representation is not defined
by the indexed/persisted page-store design above.

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
3. promotes invalid indexed/persisted page domains as usual;
4. runs ordinary reverse planning;
5. runs ordinary `tock_coverage()` evaluation for all implicated nodes; and
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

Sample indexed reads address global covered sample indices. Stored representations
provide direct/page-backed access; `indexed/ephemeral` intermediates may instead be
transaction/live transient views.

Indexed event reads may span several stored segments/pages and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Indexed accessors expose input coverage. Debug validation should catch explicit
indexed reads outside coverage. A `tock_coverage()` accessor reads only indexed
inputs from the transaction's selected indexed view. Realtime values enter that
world only through an explicit bridge node whose captured data belongs to the fixed
transaction snapshot described in sections 22 and 24. The accessor therefore never
reads mutable realtime storage or captures appended after that snapshot's cutoff.

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

For `indexed/persisted`, a newer candidate is not externally presented as a partially
materialized stored output. Until the entire required stored candidate is complete,
the newer version is pending/not-ready and callers may continue using an older
completed published result. Missing candidate pages must never be represented as
neutral samples, zero events, or artificial missing coverage.

For `indexed/ephemeral`, an external request may evaluate exact requested coverage
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
`indexed/ephemeral` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and semantic invalidation are separate events. Producing a new
`CompiledGraph` generation does not by itself make stable indexed/persisted output
stale or revoke the retention guarantee of realtime/persisted output.

`GraphExecutor` reconciles old/new generations using stable concrete-node/output
identity. Compatible persisted outputs rebind to the existing stable store rather
than copying payloads merely because machine code changed.

Important cases:

- indexed/persisted with unchanged semantics preserves coverage and payloads;
- indexed/persisted with changed semantics runs exact invalidation and rebuilds
  invalid candidate pages before publication;
- realtime/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no semantic persistent payload to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic endpoint identity rather than builder
handles, lowered node indices, or ORC-generation-local ordinals.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output realtime root. It does
not gain synthetic indexed output ports or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal indexed endpoints to
generated indexed-component executors, indexed/persisted bindings, live pull plans,
and explicit recording-bridge capture/output identities. Static indexed topology is
specialized during lowering rather than rediscovered for every request or block.

### Reuse whole-graph analysis products during lowering

The no-indexed-cycle constraint is intentionally cheap to validate and should not
be implemented as a reachability traversal from every indexed output. Whole-project
lowering already needs dependency/SCC/order information for scheduling and later
optimization, so it should compute and reuse those facts.

After connection/dependency classification, one linear-time SCC decomposition of
the complete semantic dependency graph records at least node-to-SCC membership.
The indexed-cycle rule then reduces to one linear indexed-connection scan:

```text
for every indexed connection source -> target:
    reject iff semantic_scc(source) == semantic_scc(target)
```

Checking each direct indexed edge is sufficient. The intended complexity is
`O(V + E)` for SCC construction plus `O(E_indexed)` for validation, not one
reachability search per output/connection.

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
- stable persistent indexed stores/immutable roots for `indexed/persisted`
  outputs, plus per-generation bindings;
- indexed semantic versions plus immutable pages versions and candidate/published
  snapshots;
- the recording-capture log, its processed-sequence frontier, slab/block reclamation
  state, and coordination with a non-realtime allocation worker;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/tock transactions;
- completion scheduling for candidate `indexed/persisted` outputs; and
- external indexed access/change-notification lifetime.

## 30. Deliberately open implementation/tuning choices

The following remain implementation choices rather than semantic ambiguity:

- dense versus coverage-packed stored sample payloads;
- physical implementation of quiescent block-size repaging and old-layout
  snapshot retention;
- stable arena/chunk/file/mmap representation for complete stored outputs;
- exact immutable root/page-directory representation and structural sharing;
- worker scheduling for `indexed/persisted` candidate completion;
- capture slab/block size, free-capacity target/watermarks, allocator wake-up
  strategy, and exact lock-free/RT-safe free/filled queue representation;
- external result ABI (owned result versus scoped pinned view/future);
- transaction workspace allocation/reuse strategy;
- cancellation granularity for superseded candidate work;
- genuine future multi-node `*_coverage_batch` ABI; and
- cost-model tuning, SIMD layout, fusion/direct forwarding, and other late
  GraphJit optimizations.

The following are **not** left open:

- output access and retention are independent configuration facts;
- `IndexedState` is non-semantic tock-only acceleration state;
- computed-output forward coverage is exact and requires an explicit semantic
  provider;
- reverse propagation is value-blind and may conservatively over-request;
- a published `indexed/persisted` output is complete over its entire coverage;
- persistent stored page width equals the canonical power-of-two whole-graph root
  block size and shares its absolute-sample-zero alignment;
- changing that block/page quantum is a quiescent lossless layout migration, not an
  indexed semantic change by itself;
- realtime and indexed ports do not implicitly connect across execution domains;
- realtime-to-indexed transfer is represented by explicit bridge nodes;
- recording capture records carry insertion sequence, output-port identity, and
  global block position, with no monotonic-position requirement;
- each indexed pass fixes its capture-sequence cutoff before propagation and never
  admits later captures into that pass;
- capture insertion does not publish indexed pages; one pages version is published
  only after the full propagation/reverse/tock transaction commits;
- published indexed versions never reference capture storage, so committed capture
  blocks can be reclaimed independently of indexed reader pins; and
- no indexed edge may participate in an SCC cycle.

## 31. Static validation

Node/package validation should reject or diagnose at least:

- invalid `IndexedState` type shape;
- an `IndexedState` contract that exposes it to tick/forward/reverse callback APIs;
- `tock_coverage()` without any `indexed/ephemeral`/`indexed/persisted` indexed output;
- any computed indexed output without `tock_coverage()`;
- any computed indexed output without an exact `propagate_forward_coverage()` (or
  future equivalent exact static/batched coverage provider);
- `realtime/persisted` output writes attempted from `tock_coverage()`;
- computed indexed output writes attempted from `tick_block()`;
- invalid/non-finite `max_events_per_index` declarations;
- indexed reads outside exact input coverage where validation can observe them; and
- producer/configuration combinations that cannot be represented by the factored
  access/retention contract.

Whole-project GraphJit validation runs after all project connections and explicit
feedback semantics are known. It computes semantic SCC membership over the
complete logical dependency relation and rejects **every indexed connection whose
source and target are in the same semantic SCC**, including indexed self-loops.
Diagnostics should identify the indexed edge and SCC participants/path witness
where practical.

The compiler should additionally verify that ordinary realtime event fan-in
capacities are sufficient for all declared incoming bounds and that explicit
recording bridges have valid realtime/indexed port shapes. Capture backlog storage
is dynamically slab-provisioned off the audio thread and is not required to fit in
the fixed whole-graph frame layout.

## 32. Implementation landing order

The existing API can move to the factored model without changing every indexed
execution mechanism at once:

1. **Factor output access from retention.** Keep `RealtimeOutputConfig` and
   `IndexedOutputConfig`; move `OutputRetention::{ephemeral,persisted}` onto the
   parent output config. Preserve the existing input configs. Update configured-
   graph serialization, compiler records, and validation to carry these facts
   independently.
2. **Keep the existing indexed F/R/T plan.** Dense indexed node/endpoint ordinals,
   semantic SCC facts, exact forward/reverse orders, coverage accumulators, stable
   endpoint identity, and callback ABI remain useful for `IndexedOutputConfig`.
   Replace producer-mode tests with access/retention tests.
3. **Add explicit recording/capture bridge nodes.** Keep direct port compatibility
   within the realtime or indexed domain. A bridge's realtime path captures each
   recording output block at its production point into pre-provisioned slab storage,
   tagging it with capture sequence, output-port ID, and global block position.
4. **Add independent capture provisioning and transactional consumption.** Run a
   non-realtime allocation worker that maintains target free capacity independently
   of tock progress. Each indexed pass snapshots a fixed capture-sequence prefix,
   seeds ordinary invalidation from those records, runs normal F/R/T work, publishes
   one successor pages version, and only then reclaims the committed capture blocks.
5. **Finish indexed execution/publication.** Indexed/ephemeral demand uses transient
   result storage; indexed/persisted candidates become visible only when their
   entire exact coverage is materialized. Keep stale-work rejection, semantic
   versions, stable rebinding, and safe publication.
6. **Optimize only after the semantic compiler surface is complete.** Then add
   genuine multi-node `*_coverage_batch` callbacks, topology-permitted tick
   grouping, persistent-backing alternatives, workspace liveness packing,
   SIMD/vectorization/fusion, and the value-specialization work described in the
   GraphJit direction.

A bridge capture sequence is not a timeline sequence. Seeking may append new blocks
for previously processed global positions; those captures simply invalidate and
recompute the affected indexed regions in a later transaction. The monotonic
frontier is the processed capture sequence, not a global sample position.

## 33. Summary invariants

1. `IndexedOutputConfig` describes globally addressed, order-independent
   `tock_coverage()` production; `RealtimeOutputConfig` describes sequential
   `tick_block()` production with finite history/latency semantics.
2. Output access and `OutputRetention::{ephemeral,persisted}` are independent.
   Retention never selects the producer callback and does not imply cross-domain
   accessibility.
3. `RealtimeInputConfig` consumes realtime outputs and `IndexedInputConfig`
   consumes indexed outputs. Realtime-to-indexed transfer requires an explicit
   bridge node.
4. Recording bridges capture blocks at the recording output's production point
   during `tick_block()`, not in a graph-wide end-of-tick sweep.
5. Every captured block carries a monotonically increasing capture sequence, an
   output-port ID, and a global block position. Capture sequence orders insertion;
   global positions and port identities may be arbitrary/nonmonotonic.
6. A dedicated non-realtime allocator extends slab-backed capture storage and
   maintains target free capacity. The realtime path consumes only already-
   provisioned blocks; allocator progress is independent of propagation/tock.
7. Each indexed bridge pass snapshots one fixed contiguous prefix of capture
   **sequence** at its start. Captures appended after the cutoff are invisible to
   that pass even when they target earlier global positions.
8. The fixed capture snapshot is coalesced into exact changed coverage keyed by
   output port and then enters the ordinary indexed F/R/T machinery. There is no
   special downstream recording scheduler.
9. Capture insertion is not indexed publication. One coherent pages version is
   atomically published only after the full invalidation propagation, reverse
   planning, and tock/materialization transaction succeeds.
10. Published indexed versions own/materialize their retained data and never
    reference raw capture blocks. After commit, the consumed capture interval can
    be reclaimed immediately; stale/cancelled work does not advance the processed
    capture frontier.
11. `IndexedCoverage` remains the semantic domain of indexed values. Forward
    changed regions remain exact and are never widened merely because a persistent
    page becomes invalid.
12. Indexed/persisted candidates are complete over their exact coverage before
    publication. Partial candidate validity is never externally published.
13. `IndexedState` is visible only to `tock_coverage()` and is non-semantic
    acceleration state.
14. No indexed dependency may participate in a directed semantic SCC cycle.
15. Stable indexed/persisted output storage belongs to executor-owned stable stores
    with per-generation endpoint bindings; `NodeStorage` remains fixed-layout
    runtime state/region storage.
16. Indexed event ordering and capacity remain deterministic and statically bounded
    where required by realtime execution.
17. Whole-project graph-analysis facts should be reused across compiler passes.
18. Forward and reverse indexed propagation are opposite dependency-direction
    queries, not mathematical inverses.
19. One logical indexed batch unions convergent requirements before visiting a
    node, so each applicable forward, reverse, and tock callback runs at most once
    per node for that batch.

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

> Indexed values are globally addressed, order-independent values. Indexed
> outputs publish exact finite **coverage**. Outside coverage, the output does
> not exist for indexed dependency purposes and node code may not request it.

> Every indexed output declares exactly one producer mode: `tick_record`,
> `tock_realtime`, or `tock_stored`. `tock_realtime` is demand-driven and
> realtime-compatible with no persistent output result. `tock_stored` is computed
> by `tock_coverage()` and is complete over its entire coverage before publication.
> `tick_record` is authoritative retained indexed data updated by `tick_block()`
> using whole-current-block-or-no-write semantics.

> Forward propagation records exact semantic coverage/change without forcing
> evaluation. Computed indexed outputs require an exact forward-coverage provider.
> Reverse propagation is value-blind and may conservatively over-request. Stored
> candidate pages are a recomputation granularity only; page boundaries never widen
> semantic coverage or forward changed regions.

> `IndexedState` is non-semantic acceleration state visible only to
> `tock_coverage()`. It may affect execution pace but may not affect values,
> coverage, dependency requirements, or any observable result.

> No indexed dependency may participate in a directed cycle. After whole-project
> semantic SCC detection, every indexed connection must strictly leave its source
> node's semantic SCC. Realtime SCCs may export indexed data outward, but indexed
> edges never participate in cyclic execution semantics.

> One live root-graph invocation captures an immutable published indexed base.
> Causally downstream generated live code may additionally observe complete
> `tick_record` current-block staging produced earlier in that invocation. UI and
> asynchronous indexed requests never observe mutable recorder staging.

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
  semantic version;
- **payload**: retained sample/event values for the page domain; and
- **semantic version**: the dependency/configuration environment for which those
  values are meaningful.

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

For a published `tock_stored` output, every page with a nonempty page domain is
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
positions from a very large coverage, for example. Sparse demand behaves according
to the indexed output's **producer mode**, not according to a generic cache policy.

A `tock_realtime` output owns no persistent materialization. A sparse request is
intersected with exact coverage and stays exact through that output. Its
`tock_coverage()` work may therefore compute only the requested covered regions,
writing directly into caller/result or transaction-local storage.

A published `tock_stored` output is different: the whole output coverage has
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
candidate is valid, the `tock_stored` output is complete and that semantic version
may become publishable.

A `tick_record` output is also persistently stored, but its authoritative data is
not regenerated by `tock_coverage()`. Whole-block recorder writes replace or add
stored data directly through the recorder publication path described later.
Sparse external requests simply read the published authoritative representation.

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
intersects a `tock_stored` candidate page, the whole page becomes invalid locally,
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

A `tock_realtime` request has no such page promotion: its exact covered demand is
reverse-propagated directly.

This is the deliberate asymmetry:

- forward semantic change is never widened merely because a stored page becomes
  invalid;
- candidate completion for `tock_stored` may page-promote work because the stored
  version must become complete; and
- demand through `tock_realtime` remains exact because no retained output page is
  being completed.

## 7. Forward changes are independent of demand

Forward propagation is independent of indexed access and is not limited to
incoming indexed-edge changes.

Any event that changes a node's indexed meaning may schedule a forward update,
including:

- editing a control point or other semantic node configuration;
- replacing/importing a resource;
- changing configuration whose effect is region-local or global;
- a published `tick_record` whole-block replacement; or
- another executor-mediated semantic mutation.

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
For `tock_stored`, every candidate page intersecting `changed`, an added/removed
page domain, or another representation-invalidating change becomes invalid in the
candidate. The exact `changed` set continues downstream unchanged by page
boundaries.

Forward processing does not synchronously call `tock_coverage()`. It may schedule
candidate completion for affected `tock_stored` outputs, but actual evaluation is
a separate operation and may run asynchronously.

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
inputs are accumulated/unioned before the node is visited, so a node should
normally participate once per forward transaction.

For every node that declares at least one `tock_realtime` or `tock_stored` indexed
output, an explicit forward-coverage implementation (or a future equivalent
multi-node/batched implementation) is **mandatory**. Coverage is semantic and exact;
there is no generally correct fallback that can invent it. A framework may still
synthesize trivial glue when exact coverage is mechanically declared by another
static facility, but it must not silently preserve old/empty coverage or substitute
a conservative superset.

A node whose indexed outputs are exclusively `tick_record` does not need a dummy
forward callback merely to publish the coverage of its own recorder writes. The
authoritative recorder store establishes that coverage directly. If such a node
also has computed outputs, the callback is required for those computed outputs.

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
downstream paths are accumulated/unioned before the node is visited.

For `tock_realtime`, the output requirement is the exact covered demand. For
`tock_stored`, candidate completion first selects the complete covered domains of
invalid stored pages, and those selected domains become the requirements supplied
to reverse planning.

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

For a `tock_realtime` output that participates in a live pull path, any reverse
mapping that must execute dynamically on the audio thread is part of the
realtime-safety contract. GraphJit should specialize/precompute such mappings
wherever possible.

`IndexedState` is **not** visible here. Dependency requirements may not depend on
memoization history.

## 10. `tock_coverage()`

`tock_coverage()` is the one-node indexed evaluation callback for outputs whose
producer mode is `tock_realtime` or `tock_stored`. Its context carries requested
`IndexedCoverage` per computed indexed output; one invocation may therefore
compute many disjoint regions and several outputs of the same node.

The callback receives only work that needs computation for the target operation:

- a `tock_realtime` output receives exact demanded covered regions; and
- a `tock_stored` output receives selected invalid candidate page domains, each
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
makes that safe. In particular, a `tock_realtime` live path may never wait on a
lock held by background/UI tock work merely to share memoization; GraphExecutor can
instead reserve a live instance, duplicate state, or otherwise provide exclusive
realtime-safe access.

Successful tock completion is transactional. For samples, every requested covered
sample/channel of every requested output is completely initialized. For events,
the callback emits the complete event sequence for every requested covered region;
zero events is a complete result. Failed, cancelled, or superseded work commits no
partial output as valid.

Within one output invocation, events are emitted in nondecreasing global timestamp
order. Equal-timestamp producer-local order is preserved.

A single callback remains preferable to mandatory per-output callbacks because a
node may share useful work among several outputs. The producer declaration still
makes a strong per-output promise: if only one `tock_realtime` output is requested,
the callback path needed for that output must always be realtime-compatible and
must be able to skip unrelated expensive outputs. If several `tock_realtime`
outputs are simultaneously requested by one generated live pull, their combined
requested path must also satisfy the realtime contract. If an output cannot meet
that promise without computing an expensive sibling, it should be `tock_stored` or
the functionality should be split into separate nodes.

The name deliberately does not contain `batch`: this is still one node. A future
`tock_coverage_batch()` may evaluate multiple nodes simultaneously, with separate
coverage and optional acceleration state for each node.

## 11. Indexed access and stored-candidate completion

There are two related but distinct executor operations: demand-driven indexed
access and completion of persistent candidate state.

### Demand-driven access

For a requested `tock_realtime` sink output:

1. intersect the request with exact output coverage;
2. keep that covered demand exact;
3. reverse-propagate required input coverage through `tock_realtime` producers
   until reaching persistent stored sources/boundaries or another exact source;
4. evaluate implicated nodes in dependency order; and
5. return/forward the requested materialization from caller, transaction, direct
   consumer, or bounded live-transient storage.

A request for a published `tock_stored` or `tick_record` output does not trigger
partial reconstruction of that output. It reads the requested subset directly
from the complete published stored representation.

### `tock_stored` candidate completion

Forward invalidation may create a candidate version with invalid stored page
domains. To make the candidate publishable, the executor:

1. gathers every invalid/nonexistent page domain required by the candidate's exact
   output coverage;
2. reverse-propagates those complete covered page domains;
3. recursively ensures upstream `tock_stored` candidates are complete for the same
   target semantic version;
4. evaluates necessary `tock_realtime` intermediates into transaction-local
   storage and `tock_stored` producers into candidate persistent storage; and
5. commits page results transactionally until the entire stored coverage is valid.

Conceptually:

```text
semantic mutation / recorder publication
        |
        v
exact forward propagation
        |
        v
candidate tock_stored pages invalidated
        |
        v
select every invalid covered page domain
        |
        v
reverse dependency planning
        |
        +-- tock_realtime -> exact transaction/live materialization
        |
        `-- tock_stored   -> candidate stored page domain
        v
forward tock evaluation
        |
        v
all stored coverage complete
        |
        v
candidate may publish at a legal whole-graph boundary
```

Whenever topology permits, each implicated node should participate once in reverse
planning and once in forward evaluation for the complete coalesced operation.
Complete stored publication does **not** require all transient intermediates or all
invalid pages to coexist in memory at once: GraphExecutor may complete a candidate
page/chunk at a time and reuse bounded transaction workspace. The semantic
requirement is that every covered `tock_stored` page/domain is valid before the
candidate becomes published, not that the entire computation is one monolithic
allocation or callback.

## 12. Forward invalidation transaction

A forward change transaction is independent of external access. It may begin from
changed indexed output regions, changed indexed input regions, a semantic
configuration mutation, a coverage update, or publication of a `tick_record`
block.

The evaluator conceptually performs:

```text
semantic mutation / changed upstream output
        |
        v
schedule affected nodes in forward order
        |
        v
accumulate exact changed input regions + current input coverage
        |
        v
propagate_forward_coverage()
        |
        +--> publish exact new computed-output coverage
        |       |
        |       `--> derive added/removed coverage as semantic changes
        |
        `--> publish exact changed output regions
                |
                v
invalidate affected tock_stored candidate pages
                |
                v
forward exact semantic changed regions again
```

Forward propagation continues through `tock_realtime` intermediates even though
they own no persistent output pages, because farther downstream stored outputs may
have been derived from an older semantic version.

The transaction does not synchronously force tock evaluation. Candidate stored
completion is scheduled separately, but every `tock_stored` output required by a
publishable candidate must eventually be complete over its full coverage.

## 13. Indexed output producer modes

An indexed output declares **how its values are produced**. This replaces the
older boolean `cache` property:

```cpp
enum class IndexedProducer {
    tick_record,
    tock_realtime,
    tock_stored,
};

struct IndexedOutputConfig {
    IndexedProducer producer = IndexedProducer::tock_stored;
};
```

The exact spelling may follow surrounding enum conventions, but these three states
are the intended semantic model. A single enum makes invalid combinations such as
"uncached but tick-written" unrepresentable.

### `producer = tock_realtime`

`tock_realtime` means:

- the output is produced by `tock_coverage()`;
- the output owns no persistent materialized result;
- exact UI/non-realtime demand writes into result/transaction storage;
- live demand may execute the required tock path on the audio thread;
- generated live materialization should use direct consumer placement or bounded
  compiler-owned transient storage whenever possible; and
- when this output is requested without unrelated outputs, all required tock and
  dynamic reverse-mapping work is always realtime-compatible.

Realtime-compatible has the same bounded hot-path meaning as `tick_block()`: no
blocking on non-realtime work, no unbounded/dynamic allocation, no unsafe locks,
and execution cost expected to keep up with realtime use. This is an authored
contract rather than something the type system can prove.

Cheap deterministic function-of-index outputs are canonical examples.

### `producer = tock_stored`

`tock_stored` means:

- the output is produced by `tock_coverage()`;
- the output has persistent stored materialization;
- its tock is never used as an audio-thread fallback;
- a published semantic version contains the output's **entire exact coverage**;
- invalid pages may exist while a candidate version is being rebuilt, but that
  candidate is not published as the stored output until all covered pages are
  valid; and
- sparse UI/live reads of a published version are reads from already-complete
  stored data, not triggers for lazy partial materialization.

This mode is appropriate when computation cannot be promised to satisfy the audio
deadline, or when retaining an exact indexed result is otherwise desirable.
Complete stored materialization is a deliberate product/architecture choice. Very
large results may later use file/mmap/compressed backing; physical backing policy
does not weaken semantic completeness.

### `producer = tick_record`

`tick_record` means:

- the output is authoritative retained indexed data;
- it is produced/updated by `tick_block()`, not by `tock_coverage()`;
- during one root graph block the producer may either replace the **entire current
  graph-block interval** or perform no write;
- a complete write may overwrite already-covered positions or establish newly
  covered positions;
- no write preserves both existing values and existing coverage over that block;
- the realtime write first lands in compiler-owned private staging; and
- persistent authoritative state is updated/versioned asynchronously from the
  completed staging block.

For event outputs, writing an empty event sequence is a complete replacement and
is distinct from performing no write.

This mode supports recorders, samplers, mutable clips, file-backed sample nodes,
and related cases where realtime execution changes persistent indexed content.
External backing-store persistence, such as rewriting a WAV file, is downstream of
the indexed-state update and must not block the audio graph.

## 14. Stored pages share the canonical whole-graph block quantum

Persistent stored outputs (`tock_stored` and `tick_record`) use canonically aligned
fixed-width pages whose width is **exactly the fixed whole-graph root block size**
for the active executable/layout generation.

Let the power-of-two root block size be `B`. Root execution and persistent stored
pages use the same absolute-sample grid, aligned to sample zero:

```text
block/page index i
    = floor(global_index / B)

root_block(i)
    = [i * B, (i + 1) * B)

page_interval(i)
    = [i * B, (i + 1) * B)

page_domain(i)
    = page_interval(i) & output.coverage
```

A root graph invocation therefore executes one canonical aligned block interval;
it does not shift the root block grid to an arbitrary transport offset. Transport,
record/punch, or similar changes that affect root execution take effect on this
canonical block grid. A host may expose or consume a narrower subrange when useful,
but the underlying generated root execution and `tick_record` transaction remain
aligned to the complete root block.

This makes one successful `tick_record` sample/event replacement map to exactly
one persistent page interval. No write leaves that page's authoritative content
unchanged. The 1:1 mapping is a physical-layout/compiler invariant; coverage
remains exact. A `tock_stored` page can therefore still have a sparse semantic
`page_domain`, while a successful `tick_record` replacement establishes coverage
over its complete current page/block interval.

For a `tock_stored` candidate, a page becomes valid only after its entire
`page_domain` has been materialized successfully for the target semantic version.
Every nonempty page domain is valid in a published stored version.

Payload representation may adapt to **coverage occupancy** inside the page:

- a dense page may store one value per physical page position for trivial direct
  indexing; or
- a coverage-packed page may store values only for positions in `page_domain`,
  using page-domain regions to map global indices into packed payload.

This sparse/dense choice concerns payload layout, not semantic coverage or
validity.

The outer page directory should preserve page-index order because indexed work is
range-oriented. Avoid moving large payloads when inserting into a flat directory:
store small handles/indices in the sorted directory and keep actual payloads in
stable arena/chunk/file-backed storage.

One `tock_coverage()` may cover many selected pages. Page boundaries must not force
one callback invocation per page.

### Changing the whole-graph block size repages stored data

Changing `B` changes the physical partition, not indexed DSP meaning. A block-size
change therefore performs a **lossless stored-layout migration** rather than an
indexed semantic invalidation or a `tock_coverage()` recomputation solely for the
new page width.

For example, `B = 256 -> 512` merges adjacent old page intervals into the new
canonical page intervals, while `B = 512 -> 256` splits each old interval. Sample
values retain their absolute indices. Coverage is unioned/intersected with the new
intervals as appropriate. Event payloads are repartitioned by absolute sample index
while preserving the deterministic indexed event order.

`tock_realtime` owns no persistent output pages and requires no repaging.
Persistent `tock_stored` and authoritative `tick_record` content must be migrated.
For `tick_record`, migration is mandatory and lossless because the authoritative
content may not be reproducible. For `tock_stored`, repaging normally reuses/copies
the already-computed values rather than rerunning DSP.

The initial implementation should make a block-size change a **quiescent graph
transition**: no concurrent `tick_record` mutation occurs while persistent stored
content is repartitioned. The executor prepares the new page roots/directories,
GraphJit prepares the new root-block-sized staging/layout for the replacement
executable generation, and the new graph/layout is published only after the
migration is complete. Old immutable snapshots may retain the old physical layout
until no reader can still observe them.

Semantic version and physical layout generation are distinct concepts. Repaging
may therefore preserve semantic version `V` while replacing layout generation
`L(B_old)` with `L(B_new)`. Runtime page handles/views must be interpreted against
the layout generation that created them; stale physical handles must not be mixed
with a newly published block/page grid.

The root block size is consequently a deliberate performance/latency knob for both
realtime execution and stored indexed layout: larger blocks reduce scheduling and
publication overhead, while smaller blocks reduce recording/update granularity and
latency. The cost of changing that knob is an explicit quiescent repaging pass.

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
audio thread. `tick_record` live staging remains bounded/preallocated from the
same static event-density declarations.

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
    IndexedProducer producer; // tock_stored or tick_record
    // Versioned coverage/page/root state and stable payload storage.
};
```

Ownership rules:

- stable `tock_stored` and `tick_record` outputs bind new executable generations to
  their existing stable stored entries when semantic compatibility permits;
- `tock_realtime` outputs own no persistent output entry to migrate or rebind;
- anonymous persistent outputs receive generation-local stored state; and
- authoritative `tick_record` data must never be silently discarded merely
  because executable code was regenerated.

Stable storage reuse is normally **rebinding**, not copying. Page directories,
persistent payloads, file-backed roots, and immutable snapshots may remain owned
by the executor while old/new executable generations refer to appropriate versions.

The three important physical forms are therefore:

```text
persistent authoritative/stored indexed representation
transaction/live transient tock_realtime materialization
compiler-owned private tick_record staging
```

All participate in the same indexed value semantics but have different ownership
and publication rules.

## 17. Indexed transaction workspace

Indexed planning/evaluation needs request-sized temporary storage that is not
necessarily bounded at graph compile time. `GraphExecutor` should own/reuse a
transaction workspace or arena containing things such as:

- forward/reverse region-set work buffers;
- selected stored-page completion plans;
- non-realtime `tock_realtime` result/intermediate sample/event values;
- temporary event payloads and segmented views; and
- temporary references to immutable stored snapshots/pages.

Transaction-local values remain shareable across all consumers in the same
logical transaction. Once their last consumer is complete, storage may be reused;
future liveness packing is an implementation optimization.

Bounded live storage, including `tick_record` staging and live `tock_realtime`
materialization, is planned by GraphJit and must not depend on request-sized dynamic
allocation from the audio thread.

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

This rule applies equally to `tick_record`, `tock_realtime`, and `tock_stored`.
Indexed access therefore never introduces a second fixed-point/feedback model and
never participates in the cyclic sequential execution semantics used for realtime
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

For a new node with computed indexed outputs (`tock_realtime` or `tock_stored`):

1. ordinary node configuration/resources and optional `IndexedState` are
   initialized;
2. current indexed-input coverages are available once upstream coverage is known;
3. `propagate_forward_coverage()` runs with a node-created/local-change cause and
   may have zero changed indexed-input regions; and
4. exact computed-output coverage is established before demand may target it.

For a newly created computed output, old coverage is empty, so all new coverage is
an exact semantic output change and propagates downstream.

A `tick_record` output obtains exact coverage from its authoritative stored
representation. That representation may begin empty, be initialized/imported from
a resource such as an audio file, or be restored from persistent project data.
Such initialization is an executor-mediated authoritative mutation and supplies
its coverage/change information directly. Later tick writes extend/replace that
representation one complete current block at a time.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Compatible stored state and
coverage are rebound unless semantic configuration, indexed connection sets,
implementation semantics, sample rate, or another explicit forward cause requires
recomputation.

A `tock_stored` candidate must become complete over its full new coverage before
publication. A `tock_realtime` output needs no persistent precomputation.

## 20. Executor-controlled semantic mutation entry points

Any mutation that affects published indexed semantics enters through an
executor-controlled boundary. Code outside `GraphExecutor` must not mutate active
persistent indexed roots/pages behind the executor's versioning rules.

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
complete affected tock_stored outputs
```

The mutation may have zero changed indexed inputs and still change output coverage
or values.

An indexed **connection-set change** is another executor-controlled forward cause.
Addition, removal, replacement, or another semantic change to the set of
connections feeding one indexed input conservatively marks that whole logical
input as changed:

```text
changed_input = old_input_coverage | new_input_coverage
```

The node sees the new current input coverage plus that changed region. This may
overinvalidate unaffected fan-in portions but is finite, simple, and correct.

Realtime-originated `tick_record` mutation uses the bounded staging/publication
handoff in section 24. The audio path does not traverse dynamic persistent indexed
structures or perform candidate graph propagation itself.

## 21. Indexed semantic versions and stale-work rejection

Indexed computation may overlap realtime execution and may be superseded by newer
edits. Persistent computed state is therefore relative to an indexed **semantic
version**.

A tock/candidate-completion transaction is tagged with the target version against
which its coverage/configuration/dependencies were planned. Results commit only to
that candidate. Superseded work may be cancelled or allowed to finish, but it must
not make a newer candidate valid accidentally.

For `tock_stored`, a candidate semantic version is publishable only when every
covered page domain of every required stored computed output is valid for that
candidate. Unchanged pages may be structurally shared from an older immutable
snapshot.

For `tock_realtime`, no persistent output validity exists; the semantic version
selects the configuration, coverage, sample rate, and stored upstream data against
which the exact request is evaluated.

`tick_record` writes create authoritative stored changes through their own staged
commit path. Until those changes are published, external/background indexed work
continues to observe an older immutable published snapshot.

Within a candidate, mutation/planning/commit ordering must prevent stale work from
being labelled current. Version/epoch validation at commit is the minimum
correctness mechanism when evaluation runs concurrently.

## 22. Realtime indexed reads: published base plus causal live overrides

Indexed-to-realtime lowering depends on producer mode:

- `tock_realtime` may execute `tock_coverage()` inline on the audio thread using
  compiler-planned direct/transient storage;
- `tock_stored` reads from complete published persistent storage and never executes
  its tock as an audio-thread fallback; and
- `tick_record` reads normally come from published authoritative storage, except
  that causally downstream live code in the same root graph invocation may observe
  the producer's complete current-block staging replacement.

For an indexed input projected onto the current realtime block:

- inside coverage, values/events come from the appropriate published stored data,
  current-pass `tick_record` override, or inline `tock_realtime` result;
- outside coverage, ordinary realtime current-block access yields the input's
  declared `neutral_value` or no events without creating indexed storage; and
- arbitrary explicit indexed reads remain constrained to input coverage.

### Live pull lowering through `tock_realtime`

GraphJit lowers exact live pull paths backward through `tock_realtime` outputs
until persistent stored sources/boundaries are reached:

```text
realtime input
      ^
      |
tock_realtime indexed output  -> inline tock
      ^
      |
tock_realtime indexed output  -> inline tock
      ^
      |
tock_stored/tick_record       -> published persistent boundary
```

No dynamic indexed page allocation occurs for the `tock_realtime` outputs. With
one compatible no-history consumer, the producer may write directly into that
consumer's representation; otherwise GraphJit uses bounded transient storage with
ordinary fanout/liveness reuse.

### Live visibility of `tick_record`

A whole live graph invocation captures one immutable published indexed **base
snapshot**. Causally downstream generated live execution may additionally observe
complete `tick_record` replacements produced earlier in the same invocation.
These replacements are private live overlays, not a generally published indexed
version.

For current block `B`:

```text
if tick_record wrote B:
    causally downstream live readers use staged B
else:
    live readers use published base data/coverage for B
```

UI requests, background indexed transactions, and unrelated asynchronous tocks
never observe the mutable staging frame. They operate against an immutable pinned
published/candidate snapshot according to the executor transaction.

Because no indexed edge may participate in an SCC, same-pass indexed forwarding
always has an acyclic causal producer-before-consumer order. A signal that needs
ordinary realtime cyclic semantics uses realtime ports instead.

### Publication boundaries

Persistent snapshot/root publication and executable-generation activation occur
only at whole-root-graph `tick_block()` boundaries, never between primitive node
ticks. Old immutable snapshots remain alive until no live pass or external scoped
reader can observe them.

A semantic configuration change may therefore prepare a complete candidate while
the live graph continues using the old published base. A `tick_record` current-pass
overlay is the only intentional live addition to that base and is visible solely
along generated downstream live paths.

## 23. Complete stored outputs and physical backing

The initial/final semantic contract deliberately avoids a partially resident
`tock_stored` output. If a stored computed output is published, its entire exact
coverage has been materialized.

This is a stronger and simpler contract than speculative prefetch:

- arbitrary live indexed reads inside coverage do not need a future-demand
  declaration;
- sparse UI requests never discover a semantically missing stored page;
- no cache-readiness underrun exists merely because the playhead jumped; and
- correctness does not depend on lookahead, prediction, or eviction policy.

Candidate versions may of course contain invalid pages while being rebuilt, but
such a candidate is not the published stored output. The UI may keep displaying a
completed older version until the candidate is complete.

Complete materialization does not require all bytes to remain in anonymous RAM.
Future physical optimizations may use mmap/file backing, compression, deduplicated
immutable pages, copy-on-write roots, or other stable representations. If a
physical mechanism can page-fault or block, GraphJit/GraphExecutor must ensure the
specific representation used by the realtime thread satisfies realtime-access
requirements. That is a backing-store concern rather than a semantic partial-cache
model.

`tock_realtime` intentionally remains the opposite case: no persistent result,
exact demand, and immediate reclamation/reuse after consumers finish.

## 24. `tick_record` staging, live forwarding, and persistent publication

Realtime-to-indexed mutation is explicit through `producer = tick_record`. There
is no implicit realtime-to-indexed connection transport.

For one fixed compiled graph revision, GraphJit knows exactly how many
`tick_record` outputs exist and the maximum payload needed by each one for one root
block. With fixed root block size it can therefore compute the complete staging
layout at compile time:

```text
TickRecordFrame
    written bit for output 0
    fixed/bounded payload for output 0
    written bit for output 1
    fixed/bounded payload for output 1
    ...
```

Sample capacity comes from block size and channel/layout facts. Event capacity
comes from block size and the effective `max_events_per_index` bounds. No staging
allocation is required during or between root `tick_block()` calls.

The baseline implementation may allocate two whole-graph frames and swap ownership
at root-block boundaries:

```text
block N:
    audio writes frame A
    publisher consumes frame B

boundary:
    swap frame pointers

block N+1:
    audio writes frame B
    publisher consumes frame A
```

Triple buffering is an optional robustness/performance variation, not a semantic
requirement. With double buffering the publisher has one whole root-block period
to make the completed frame reusable. A missed publication/reuse deadline must be
detected as a bounded recorder-publication overrun; the audio thread must not
repair it by blocking or dynamically allocating.

### Whole-block-or-none writes

For current root block `B = [begin,end)`, each `tick_record` output does exactly one
of:

```text
no write:
    authoritative values(B)   unchanged
    authoritative coverage(B) unchanged

complete write:
    values(B) replaced atomically
    coverage includes all of B
```

A sample write initializes every channel/sample of `B`. An event write supplies
the complete event sequence for `B`; an empty sequence is a valid replacement.
The node-facing API should make partial block initialization difficult, for example
by obtaining/committing one complete writable block rather than exposing an
implicitly partial per-sample dirty model.

Primitive maximum-block slicing must not weaken this root-invocation transaction.
If GraphJit invokes a recorder-producing primitive in smaller slices, lowering must
still ensure that the port's externally meaningful result for the root block is
either one complete replacement or no replacement; slice-local partial recorder
commits are not legal. The exact writer ABI/lowering mechanism remains an
implementation choice (for example, a root-scoped recorder facade accumulated by
slice calls, or a static restriction/normalization on recorder-producing
primitives).

This permits a sampler/file-backed node to replay already-stored data without
rewriting it when no realtime input is present, while still allowing an optional
realtime input to replace the current block on the fly.

### Two destinations, one logical produced block

A complete recorded block has two logical destinations:

```text
                         +--> private live block for same-pass consumers
recorded current block --+
                         `--> next authoritative stored version
```

GraphJit may avoid a physical copy when lifetimes and representations permit; the
important distinction is visibility.

The private block may be wired directly into causally downstream live indexed or
realtime consumers during the same root invocation. It is never visible to UI or
asynchronous sparse indexed requests while the producer is writing it.

The completed frame is also handed to non-realtime publication code, which updates
the authoritative persistent representation, derives exact changed/coverage
regions, constructs the next immutable stored snapshot/root, and makes that state
available for publication at a whole-graph boundary. File/device persistence may
be queued separately after the in-memory authoritative update.

Thus UI access cannot race the recorder write: UI sees the previous immutable
published snapshot until a complete new authoritative version is published, while
generated live consumers may exploit the current staged block immediately.

## 25. Indexed event/sample reads from node callbacks

Sample indexed reads address global covered sample indices. Stored representations
provide direct/page-backed access; `tock_realtime` intermediates may instead be
transaction/live transient views.

Indexed event reads may span several stored segments/pages and therefore use a
segmented ordered iterator/range rather than requiring contiguous storage. The
iterator preserves global timestamp/source/local order and never exposes or
requests outside input coverage.

Both tick and tock accessors expose input coverage. Debug validation should catch
explicit indexed reads outside coverage. During live execution, the accessor may
resolve the current block through a causally prior `tick_record` overlay while
other positions continue to resolve through the captured published base snapshot.

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

Every externally returned indexed result is associated with the indexed semantic
version from which it was read.

For `tock_stored`, a newer candidate is not externally presented as a partially
materialized stored output. Until the entire required stored candidate is complete,
the newer version is pending/not-ready and callers may continue using an older
completed published result. Missing candidate pages must never be represented as
neutral samples, zero events, or artificial missing coverage.

For `tock_realtime`, an external request may evaluate exact requested coverage
against a selected immutable semantic version and return that version with the
result. The host API may satisfy the request synchronously or expose explicit
pending/future behavior; the exact ABI is implementation work.

A caller must not combine independently returned data from different semantic
versions as if it were one coherent indexed result.

## 27. Outward change notification

After an indexed candidate or authoritative recorder update becomes a coherent
published version, the executor may emit a coalesced indexed-output change
notification for subscribed external consumers.

Conceptually it contains:

```text
output identity
indexed semantic version/revision
new coverage (or a coverage-changed indication)
exact changed IndexedCoverage
```

Notification is emitted from the executor publication/change boundary, not
recursively from individual callbacks. Presentation/UI code can react by issuing
ordinary indexed access requests. The notification does not itself force
`tock_realtime` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and indexed semantic invalidation are separate events. Producing a
new `CompiledGraph` generation does **not** by itself make stable persistent
indexed outputs stale.

When `GraphExecutor` receives a candidate executable generation it reconciles old
and new descriptions using stable concrete-node/output identities. New generation
local endpoint ordinals bind to existing stable persistent entries whenever those
identities and producer/representation semantics remain compatible. Stored page
payloads/roots are reused by ownership/reference, not copied merely because
machine code changed.

Important cases:

- **retained `tock_stored`, unchanged semantics:** preserve coverage and complete
  stored payloads;
- **retained `tock_stored`, semantic/input change:** preserve compatible immutable
  allocations/pages where possible, run exact forward invalidation, and rebuild
  every invalid candidate page before publication;
- **retained `tick_record`:** preserve authoritative recorded content across JIT
  generations; graph recompilation alone must never erase it;
- **retained `tock_realtime`:** no persistent output payload exists to migrate;
- **new computed node/output:** establish exact coverage through the node-creation
  forward rule, then fully materialize any `tock_stored` output before publishing
  the candidate;
- **new/restored `tick_record`:** initialize its authoritative stored
  representation/coverage from empty state or explicit imported/project data;
- **removed output:** old immutable snapshots remain alive only while old executable
  generations/readers can observe them; and
- **producer-mode change:** rerun static validation and reconcile storage
  conservatively. In particular, authoritative `tick_record` content must never be
  silently discarded as though it were a regenerable cache.

Connection comparisons use stable semantic endpoint identity rather than builder
handles, lowered node indices, or ORC-generation-local ordinals. An unrelated JIT
edit causes zero indexed recomputation in unaffected components.

The project sample rate is part of the computed indexed semantic environment.
Changing it invalidates/repropagates computed (`tock_realtime`/`tock_stored`)
semantics as appropriate; `tock_stored` results are recomputed before publication.
Authoritative `tick_record` sample data is **not** automatically resampled or
reindexed. The same stored samples are interpreted at the new project rate, so
playback duration/pitch may change. Preserving original timing is an explicit DSP
choice, for example by inserting/configuring a resampling node with the source and
target rates.

Old/new executable generations and old/new immutable indexed snapshots may coexist
briefly while a live block finishes and safe-boundary activation/publication occurs.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output realtime root. It does
not gain synthetic indexed output ports or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal indexed endpoints to
generated indexed-component executors, persistent bindings, live pull plans, and
`tick_record` staging offsets. Static indexed topology is specialized during
lowering rather than rediscovered for every request or block.

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
component planning, live scheduling, liveness/storage planning, producer-mode
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
- stable persistent indexed stores/immutable roots for `tock_stored` and
  `tick_record` outputs, plus per-generation bindings;
- indexed semantic versions and candidate/published snapshots;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/tock transactions;
- completion scheduling for candidate `tock_stored` outputs;
- `tick_record` staging publication/deferred reclamation; and
- external indexed access/change-notification lifetime.

## 30. Deliberately open implementation/tuning choices

The following remain implementation choices rather than semantic ambiguity:

- exact enum spelling/serialization ABI for `IndexedProducer` while the provisional
  boolean API is replaced;
- dense versus coverage-packed stored sample payloads;
- physical implementation of quiescent block-size repaging and old-layout
  snapshot retention;
- stable arena/chunk/file/mmap representation for complete stored outputs;
- exact immutable root/page-directory representation and structural sharing;
- worker scheduling for `tock_stored` candidate completion;
- double versus triple `tick_record` staging after measuring publication jitter;
- exact bounded recorder-publication overrun reporting/recovery policy;
- external result ABI (owned result versus scoped pinned view/future);
- transaction workspace allocation/reuse strategy;
- cancellation granularity for superseded candidate work;
- genuine future multi-node `*_coverage_batch` ABI; and
- cost-model tuning, SIMD layout, fusion/direct forwarding, and other late
  GraphJit optimizations.

The following are **not** left open:

- indexed output producer has exactly the three semantic modes above;
- `IndexedState` is non-semantic tock-only acceleration state;
- computed-output forward coverage is exact and requires an explicit semantic
  provider;
- reverse propagation is value-blind and may conservatively over-request;
- a published `tock_stored` output is complete over its entire coverage;
- persistent stored page width equals the canonical power-of-two whole-graph root
  block size and shares its absolute-sample-zero alignment;
- changing that block/page quantum is a quiescent lossless layout migration, not an
  indexed semantic change by itself;
- a `tick_record` write is current-whole-block-or-none;
- recorder staging is private from UI/background requests while remaining usable
  by causally downstream generated live code; and
- no indexed edge may participate in an SCC cycle.

## 31. Static validation

Node/package validation should reject or diagnose at least:

- invalid `IndexedState` type shape;
- an `IndexedState` contract that exposes it to tick/forward/reverse callback APIs;
- `tock_coverage()` without any `tock_realtime`/`tock_stored` indexed output;
- any computed indexed output without `tock_coverage()`;
- any computed indexed output without an exact `propagate_forward_coverage()` (or
  future equivalent exact static/batched coverage provider);
- `tick_record` output writes attempted from `tock_coverage()`;
- computed indexed output writes attempted from `tick_block()`;
- partial `tick_record` block writes/commits;
- invalid/non-finite `max_events_per_index` declarations;
- indexed reads outside exact input coverage where validation can observe them; and
- producer/configuration combinations that cannot be represented by the three-mode
  enum contract.

Whole-project GraphJit validation runs after all project connections and explicit
feedback semantics are known. It computes semantic SCC membership over the
complete logical dependency relation and rejects **every indexed connection whose
source and target are in the same semantic SCC**, including indexed self-loops.
Diagnostics should identify the indexed edge and SCC participants/path witness
where practical.

The compiler should additionally verify that generated `tick_record` staging fits
the statically computed whole-graph frame layout and that event fan-in capacities
are sufficient for all declared incoming bounds.

## 32. Implementation landing order

The repository currently contains the first indexed-port API/wiring with the older
provisional boolean-cache model. The next API landing should replace that
provisional shape directly rather than accumulate compatibility aliases around it.

A practical dependency order is:

1. **Producer-mode API correction.** Replace `IndexedOutputConfig { bool cache; }`
   with the three-state `producer` declaration; expose sample rate in indexed
   semantic callbacks; remove `IndexedState` from tick/forward/reverse contexts;
   require exact forward coverage for computed outputs; and add a block-transactional
   `tick_record` output writer to `TickContext`.
2. **Compiler-record/static validation update.** Serialize/scan producer mode,
   enforce callback requirements and write authority, carry tock-only
   `IndexedState`, and reject any indexed edge participating in a semantic SCC.
3. **Stable identity and static indexed planning.** Thread stable project
   instance/virtual-node/member/output identities into GraphJit; precompute indexed
   components, endpoint ordinals, forward/reverse/evaluation orders,
   convergence/conversion facts, producer modes, connection fingerprints, and
   whole-graph fixed `tick_record` staging layout.
4. **Non-realtime `GraphExecutor` capability.** Add active/pending generations,
   canonical `NodeStorage`, stable persistent indexed stores, transaction
   workspaces, semantic versions, node creation/connection reconciliation, exact
   forward invalidation, reverse demand, tock evaluation, full `tock_stored`
   candidate completion, stale-work rejection, and versioned external results.
5. **Indexed-to-realtime live lowering.** Bind complete published `tock_stored` and
   authoritative `tick_record` bases; lower inline exact pulls through
   `tock_realtime`; reuse direct/transient sample/event storage planning; apply
   neutral/no-event behavior only outside coverage; and expose current-pass
   `tick_record` blocks to causally downstream live consumers.
6. **`tick_record` publication.** Allocate double-buffered whole-graph staging
   frames from GraphJit layout facts; swap frame ownership at root block
   boundaries; publish authoritative recorder updates off the audio thread;
   construct immutable next-version roots; defer reclamation off-thread; and expose
   coalesced versioned change notifications to UI consumers.
7. **Outward migration/cleanup.** Once indexed executor/query/visualization paths
   replace legacy compiled-lane consumers, delete the old compiled-lane execution,
   storage, RPC, and UI model rather than adapting it into a second indexed system.
8. **Optimization after capability.** Add genuine multi-node `*_coverage_batch`
   operations, topology-permitted `tick_block_batch` grouping, better persistent
   backing choices, workspace liveness, SIMD-aware layout/copy/conversion,
   vectorization, fusion/direct forwarding, target-specific lowering, and generated
   hot-path assembly verification.

Stable endpoint identity must exist before persistent stored output ownership is
bound across generations. `tock_stored` full materialization and immutable
published bases must exist before arbitrary live indexed reads rely on them.
`tick_record` remains an explicit producer mode rather than an implicit fifth
connection transport.

## 33. Summary invariants

1. `indexed` describes globally addressed, order-independent sample/event data; it
   does not mean JIT compilation or a storage class.
2. `IndexedCoverage` is the sole semantic domain boundary; there is no indexed
   extent/bounding-hull abstraction.
3. Coverage is a canonical union of sorted, nonempty, disjoint, non-adjacent
   half-open regions.
4. Explicit indexed reads never request values outside input coverage; ordinary
   realtime projection outside coverage yields the input neutral value/no events.
5. Indexed input coverage is the union of connected/mapped indexed output
   coverages and is visible to tick and tock code.
6. Indexed outputs declare one producer mode: `tick_record`, `tock_realtime`, or
   `tock_stored`; the old boolean `cache` contract is obsolete.
7. `tock_realtime` is demand-driven, owns no persistent result, and promises that
   requested live tock/reverse work is always realtime-compatible.
8. `tock_stored` is produced by tock and is complete over its entire exact coverage
   before a semantic version containing it may be published.
9. `tick_record` is authoritative retained indexed data written by `tick_block()`;
   each root block either replaces the whole current interval or performs no write.
10. `IndexedState` is visible only to `tock_coverage()` and is non-semantic
    acceleration/memoization state. Its contents may affect pace, never results or
    dependency semantics.
11. `propagate_forward_coverage()` (or an exact equivalent) is mandatory for
    computed indexed outputs because output coverage cannot be safely guessed.
12. `propagate_reverse_coverage()` is value-blind; data-dependent addressing uses a
    conservative requirement superset in the initial design.
13. Forward changed regions remain exact and are never widened to page boundaries.
14. `tock_stored` candidate invalidation/completion may operate at complete covered
    page-domain granularity, but partial candidate validity is never exposed as a
    published stored output.
15. Successful sample tock completely initializes every requested sample/channel;
    successful event tock produces the complete ordered event set; failed or
    superseded work commits no partial validity.
16. Stored sample/event pages may use dense, packed, arena, mmap, file-backed, or
    other physical representations without weakening complete published semantics.
17. Indexed event ordering is deterministic by absolute sample index, stable
    source/connection ordinal, then producer-local order; capacities account for
    complete legal fan-in.
18. Stable `tock_stored` and `tick_record` output storage belongs to an
    executor-owned stable indexed store with per-generation endpoint bindings;
    `tock_realtime` owns no persistent output payload.
19. `NodeStorage` holds fixed-layout state/regions; `IndexedState` is tock-only
    acceleration state, not authoritative indexed-output storage.
20. No indexed edge may participate in a directed cycle. Semantic SCC validation
    checks every indexed connection after the complete project graph is known.
21. A node may remain in a realtime SCC and export indexed data strictly out of
    that SCC.
22. A whole live graph invocation captures one immutable published indexed base.
    Causally downstream generated live code may additionally observe complete
    current-pass `tick_record` block overrides.
23. UI/background indexed requests never observe mutable recorder staging; they
    operate against immutable published/candidate snapshots selected by the
    executor transaction.
24. GraphJit statically sizes all `tick_record` staging for one root block and may
    double-buffer whole-graph frames so the audio thread never allocates or waits
    for persistent publication work.
25. Recorder publication updates authoritative persistent state off the audio
    thread and publishes immutable roots only at whole-root-block boundaries.
26. Project sample rate is part of computed indexed semantics. Rate changes
    invalidate/recompute computed outputs but do not automatically resample
    authoritative `tick_record` sample data.
27. Stable executable-generation replacement is rebinding, not automatic indexed
    invalidation; authoritative recorder data survives compatible JIT rebuilds.
28. Whole-project graph-analysis facts should be reused across compiler passes.
    The no-indexed-cycle check is one SCC decomposition plus one indexed-edge scan,
    not per-port reachability traversal.
29. Forward and reverse propagation are opposite graph-direction dependency
    queries, not mathematical inverses.
30. Future `*_coverage_batch` names are reserved for genuine multi-node indexed
    batching; the current `_coverage` callbacks remain one-node operations.

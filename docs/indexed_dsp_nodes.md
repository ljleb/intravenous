# Indexed DSP ports and incremental indexed evaluation

This document is the normative design for **indexed DSP ports**, their
incremental evaluation model, and the executor-side storage/publication rules
needed to make indexed data usable by both UI-style random access and realtime
processing.

`indexed` replaces the older `compiled` DSP-port terminology. `compiled` is now
reserved for actual program/JIT compilation (`GraphJit`, `CompiledGraph`, LLVM
modules, generated code, and similar concepts). Some implementation identifiers
may temporarily retain legacy names such as `CompiledPortConfig`,
`CompiledState`, `access_block[_batch]`, or `propagate_block_access[_batch]`
while the code migrates. Those names do not change the semantics described here.

The intended callback vocabulary is:

```cpp
tick_block(...);                         // sequential realtime execution
tock_region_batch(...);                  // indexed evaluation
propagate_forward_region_batch(...);     // changed inputs/state -> changed outputs/coverage
propagate_reverse_region_batch(...);     // required outputs -> required inputs
```

The central rules are:

> Indexed values are globally addressed, order-independent values. Indexed
> outputs publish exact finite **coverage**. Outside coverage, the output does
> not exist for indexed dependency purposes and node code may not request it.

> Indexed dependency propagation is exact in region space, while retained cache
> validity is page-granular. A cache page is either valid or invalid as a whole,
> but its semantic page domain is exactly `page_interval & output_coverage`; page
> boundaries never force node code to process outside real coverage.

> Forward propagation records semantic change and invalidates retained results
> without forcing evaluation. Reverse propagation starts only from demanded
> invalid pages. `tock_region_batch()` materializes the exact covered domains of
> those pages.

> Realtime processing never waits for indexed recomputation. It observes one
> immutable published indexed snapshot for an entire live graph block. Edits may
> build a newer snapshot asynchronously; publication occurs only at a whole-live-
> graph block boundary after the required indexed work for that snapshot is
> complete.

This is an incremental, demand-driven evaluation model, not a second realtime
scheduler and not a storage class.

## 1. Port model

Payload kind and access model are orthogonal. Ordinary DSP nodes therefore have
four declaration combinations:

| Port kind | Realtime access | Indexed access |
| --- | --- | --- |
| sample | bounded sequential/current-block timing | arbitrary global sample positions inside coverage |
| event | bounded sequential/current-block timing | arbitrary global event regions inside coverage |

A realtime declaration carries its finite history/latency timing contract. An
indexed declaration carries no realtime history/latency contract.

`tick_block()` is sequential realtime execution. `tock_region_batch()` is
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
connected indexed outputs. Both `tick_block()` and `tock_region_batch()` can
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

## 4. Coverage, page domains, validity, residency, and payload

For retained indexed outputs the executor distinguishes these concepts:

- **coverage**: exact regions where the output exists and may legally be
  requested;
- **page domain**: for one physical cache page, `page_interval & coverage`;
- **validity**: whether that entire page domain is current for a particular
  indexed semantic version;
- **residency**: whether optional cache backing for the page currently exists;
- **payload**: retained sample/event values for the valid page domain; and
- **semantic version**: the dependency/state version for which that validity is
  meaningful.

Coverage is exact and independent of cache page boundaries. For page width
`1024` and output coverage:

```text
{ [10, 1002) }
```

page zero has the exact semantic page domain:

```text
[0, 1024) & coverage = { [10, 1002) }
```

The node never observes `[0,10)` or `[1002,1024)` merely because the cache is
page based.

A page has one validity state. It is not partially valid. If its page domain is:

```text
{ [10,100), [500,600) }
```

then a valid page means that **both** covered regions are materialized/current.
An invalid page means none of that page domain may be relied upon as retained
current data.

An allocated page may be resident while invalid. Payload bytes from an older
semantic version may remain physically present, but validity/version metadata is
the authority. Eviction loses residency and retained validity; ordinary
invalidation need not free backing storage.

## 5. Sparse logical requests become page materialization requests

External indexed sample queries may be sparse. UI waveform rendering may ask for
one sample per display frame, for example. Sparse query positions do **not**
require sparse per-sample processing or sparse per-sample validity.

For every requested covered sample/event region, the executor identifies touched
cache pages. If a touched page is already valid for the target semantic version,
that page is a cache hit. If it is invalid or absent, the required materialized
work for that output is the page's **entire page domain**:

```text
materialization(page) = page_interval & output.coverage
```

Examples for `P = 1024`:

```text
coverage = { [10,1002) }
request  = sample 500

=> materialize { [10,1002) }
```

and:

```text
coverage = { [10,100), [500,600) }
request  = sample 20

=> materialize { [10,100), [500,600) }
```

The uncovered gap is never processed, but the whole covered domain of the
invalid touched page is materialized atomically.

For coverage spanning several pages:

```text
coverage = { [10,3000) }
```

its page domains are:

```text
page 0: { [10,1024) }
page 1: { [1024,2048) }
page 2: { [2048,3000) }
```

A request for sample `500` only touches/materializes page zero.

This gives useful behavior at all UI zoom levels:

- zoomed in, many requested samples touch many consecutive pages, so evaluation
  becomes naturally dense/sequential;
- around one requested sample per page, each touched page is computed once; and
- zoomed far out, requested samples land in widely separated pages, so only
  sparse pages are touched.

Page width is therefore an implementation tuning knob trading bounded
recomputation against directory/payload overhead. Pages are expected to be small
enough that recomputing one complete page domain after a tiny invalidation is
acceptable.

## 6. Exact propagation remains independent of page granularity

Page granularity is a cache/materialization decision only. Semantic dependency
propagation remains exact in `IndexedCoverage` region space.

If a state/input mutation semantically changes:

```text
[523,530)
```

forward propagation sends exactly `[523,530)` downstream. If that region
intersects a retained page, that whole page becomes invalid locally, but the
executor does **not** widen the changed region to the whole page before
propagating it farther.

Conversely, reverse planning starts from the exact downstream requirement. At an
upstream output cache boundary, however, touching an invalid page promotes the
work to that output's entire page domain **before** reverse propagation through
that producer, because the producer is now committed to materializing the whole
page.

Conceptually:

```text
consumer exact demand
        |
        v
producer page lookup
        |
        +-- valid touched pages -> satisfied here
        |
        `-- invalid touched pages
                |
                v
        promote to full page_domain
                |
                v
        propagate_reverse_region_batch(producer)
```

This is the deliberate asymmetry:

- forward semantic change is never widened merely because a cache page is
  invalidated;
- reverse missing demand is page-promoted at each cache/materialization boundary
  because tock will compute the full touched page domain.

## 7. Forward changes are independent of demand

Forward invalidation is independent of indexed access and is not limited to
incoming indexed-edge changes.

Any event that changes a node's indexed meaning may schedule a forward update,
including:

- editing a control point or other UI-visible node state;
- replacing/importing a resource;
- changing configuration/state whose effect is region-local or global;
- recorder/source mutation during realtime execution; or
- another asynchronous mutation that changes indexed values.

A node may therefore run its forward-region logic with **zero changed indexed
input regions** because local state changed. The callback/context must be able to
distinguish such a local-state-triggered update from the absence of work.

A forward update may change two independent things for each indexed output:

1. the output's new `IndexedCoverage`; and
2. exact regions inside the old/new coverage whose values may have changed.

The executor owns the previous coverage, so node code should normally publish the
new coverage as a whole value. The executor derives coverage changes:

```text
added   = new_coverage - old_coverage
removed = old_coverage - new_coverage
```

Both are semantic output changes for downstream propagation. They are unioned
with value-change regions explicitly reported by the node:

```text
changed = reported_value_changes | added | removed
```

Any resident page whose old/new page domain or changed region is affected becomes
invalid for the target semantic version. Coverage itself is never rounded to
page boundaries.

A node-local mutation may alternatively seed exact known changed output regions
directly when the mutation API already has that knowledge. This is an entry point
into the same downstream forward transaction, not a different evaluation model.

No `tock_region_batch()` call is implied by forward processing. Many edits may
accumulate/supersede invalidations before any indexed computation is demanded.

## 8. `propagate_forward_region_batch()`

The forward dependency callback answers:

> Given this node's current indexed-input coverages, exact changed indexed-input
> regions, current node-local indexed state, and any local-state forward-update
> cause, what are the resulting output coverages and which exact output regions
> may have changed?

Conceptually:

```text
F(node): input coverage + changed input regions + current local state
      -> output coverage + affected output regions
```

It runs in forward graph direction. Incoming changed regions from all indexed
inputs are accumulated/unioned before the node is visited, so a node should
normally participate once per forward transaction.

The callback is optional only where a conservative synthesized behavior can be
correct. A conservative fallback may retain/derive conservative output coverage
and mark all covered output regions changed when an indexed input or relevant
local state changes. Nodes implement an explicit callback when they can provide
tighter coverage/change mappings.

Forward and reverse propagation are directional dependency queries, not inverse
functions.

For an FIR-like transform of support `L`, for example:

```text
input changed [a,b)
    -> output may have changed [a,b+L-1)
```

while reverse demand maps output requirements to a different input region.

## 9. `propagate_reverse_region_batch()`

The reverse dependency callback answers:

> Given exact covered regions of this node's indexed outputs that have been
> selected for materialization, which covered regions of its indexed inputs must
> be available to compute them?

Conceptually:

```text
R(node): materialized output region sets -> required input region sets
```

It runs in reverse graph direction. Requests reaching a node through multiple
downstream paths are accumulated/unioned before the node is visited.

The callback receives output work that has already been expanded to the complete
page domains of this node's invalid touched pages. After it reports required
input regions, requirements are clipped to each input's coverage. At each
upstream output, valid touched pages terminate that part of reverse traversal;
invalid touched pages promote the requirement to their complete upstream page
domains before propagation continues farther upstream.

The callback is optional. A conservative synthesized behavior may require all
covered regions of each indexed input. Output-only indexed sources have nothing
upstream to request and therefore require no reverse callback.

## 10. `tock_region_batch()`

`tock_region_batch()` is the indexed evaluation callback.

It is called only after reverse planning has determined which output pages must
be materialized for the target semantic version. It receives exactly the union
of those pages' **covered page domains**. It does not receive:

- the caller's original blind request;
- already-valid pages;
- uncovered portions of a physical page; or
- isolated sparse UI sample positions when those positions caused a whole page
  domain to be selected.

The callback therefore may receive several disjoint regions in one batch.
Multiple covered regions from one page are all present when that page is being
materialized.

The callback:

- sees indexed inputs and indexed outputs;
- operates on covered global sample/event regions;
- cannot request indexed inputs outside their published coverage;
- cannot depend on realtime-only inputs or outputs;
- cannot depend on sequential realtime `State`;
- may use persistent indexed-domain state/workspaces; and
- must produce observable results independent of indexed request order.

After successful evaluation, each selected output page becomes valid atomically
for the target semantic version. There is no partially valid retained page.

For event outputs, rematerializing a page domain semantically replaces the old
cached event contents for that page domain; stale events must not survive when a
new evaluation emits fewer events.

The callback may have an unbatched convenience form, but framework/compiler code
should normalize to one batched entry point so all required output page domains
for a node are visible together.

## 11. Indexed access transaction

A logical indexed access may request outputs from any number of internal nodes.
Requests are grouped by indexed connected component and globally batched within
each component.

For each requested sink output:

1. intersect the request with output coverage;
2. determine which physical pages that covered request touches;
3. discard pages already valid for the target semantic version; and
4. replace each invalid touched page with its complete `page_interval & coverage`
   domain.

If every touched page is valid, the access is a cache hit: there is no reverse
propagation and no tock execution.

Otherwise:

```text
requested sink positions/regions
        |
        v
intersect exact sink coverage
        |
        v
map to touched sink pages
        |
        +-- valid pages -> satisfied
        |
        `-- invalid pages -> full covered page domains
                                |
                                v
reverse planning in precomputed reverse order
        |   requirements are unioned at convergence points
        |   clipped to exact input coverage
        |   valid upstream pages terminate traversal
        |   invalid upstream pages expand to their page domains
        v
complete missing dependency plan
        |
        v
forward evaluation in dependency order
        |
        v
tock_region_batch() for selected covered page domains only
        |
        v
commit complete page results for the target semantic version
        |
        v
retain or discard payload according to cache policy
        |
        v
return requested sink values/events
```

Whenever topology permits, each implicated node participates once in reverse
planning and once in forward execution for the complete component transaction.

## 12. Forward invalidation transaction

A forward change transaction is independent of access.

It may begin from changed indexed output regions, changed indexed input regions,
a node-local state mutation, or a coverage update. The evaluator conceptually
performs:

```text
arbitrary mutation / changed upstream output
        |
        v
schedule affected node(s) in forward order
        |
        v
accumulate exact changed input regions + current input coverage
        |
        v
propagate_forward_region_batch()
        |
        +--> publish exact new output coverage
        |       |
        |       `--> derive added/removed coverage as semantic changes
        |
        `--> publish exact changed output regions
                |
                v
invalidate every retained page whose page domain intersects the change
                |
                v
forward the exact semantic changed regions again
```

Forward propagation must continue even when an intermediate has no resident
cached page for the affected region. Farther downstream retained results may
still have been derived from an older version.

No indexed evaluation is forced by this transaction. Later demand sees the new
semantic version and materializes only touched invalid pages.

## 13. Cache/materialization policy

Caching is a physical policy, not indexed semantics. Every indexed output should
have a policy with at least:

```cpp
enum class IndexedCachePolicy {
    automatic,
    always,
    never,
};
```

`automatic` should be the declaration default so most nodes require no explicit
cache annotation.

Semantics:

- `never`: do not retain an additional executor cache across transactions;
- `automatic`: retain opportunistically under executor policy/budget and allow
  ordinary eviction; and
- `always`: retain materialized output pages across transactions until semantic
  invalidation, generation destruction, or an explicitly defined exceptional
  memory-pressure policy removes them.

`never` does **not** mean duplicate computation for every consumer. One indexed
transaction may materialize transaction-local data once and share it across all
downstream consumers. It only means that result need not survive into the next
transaction.

Likely examples:

| Output kind | Typical policy |
| --- | --- |
| cheap deterministic CBRNG/function-of-index source | `never` |
| recorder already backed by authoritative node storage | often `never` for an additional executor cache |
| memory-mapped/raw source with direct stable backing | `never` or `automatic` |
| expensive decoder/source | `automatic` or `always` |
| indexed transform of indexed inputs | usually `automatic` |
| expensive repeatedly viewed analysis result | `automatic` or `always` |

Correctness must never depend on retention. Automatic promotion/demotion,
reuse-based heuristics, and exact eviction algorithms are tuning work.

## 14. Sample cache pages

Retained indexed sample outputs use canonically aligned fixed-width pages rather
than arbitrary-offset overlapping blocks.

For power-of-two page width `P`:

```text
page_index    = global_index >> page_bits
page_interval = [page_index * P, (page_index + 1) * P)
page_domain   = page_interval & output.coverage
```

Every global sample index therefore maps to exactly one page. Page width is an
implementation tuning parameter and does not alter coverage semantics.

Each retained page needs one validity/version state, not a `P`-bit validity
bitmap. A page becomes valid only after its entire `page_domain` has been
materialized successfully for the target semantic version.

Payload representation may still adapt to **coverage occupancy** inside the page:

- a dense page may store one value per physical page position for trivial direct
  indexing; or
- a coverage-packed page may store values only for positions in `page_domain`,
  using the page-domain regions to map global indices into packed payload.

This sparse/dense choice is about payload storage, not validity. It is especially
useful when one small covered region occupies a page or several small coverage
regions share one page.

A simple initial implementation may use dense payloads with a modest page width.
The page abstraction should hide that choice so packed payloads can be added after
profiling without changing node semantics.

The outer page directory should preserve page-index order because indexed work is
range-oriented. A flat/sorted directory is preferable to depending on unordered-
map iteration. Avoid moving large page payloads when inserting into a flat
directory: store small page handles/indices in the sorted directory and keep
actual page payloads in stable arena/chunk storage. `std::flat_map` is a C++23
option where toolchain/library support is suitable; a small custom sorted flat
directory is also reasonable.

One `tock_region_batch()` may cover many selected pages. Page boundaries must not
force one callback invocation per page.

## 15. Event cache pages and indexed event iteration

Indexed events use the same page validity model with a packed event payload.

For one page:

```cpp
struct EventPage {
    PageVersion valid_version; // or equivalent valid/invalid state
    PackedEventStorage events; // ordered actual events only
};
```

A valid event page means the **complete event set for every position in the
page's exact covered page domain is known**, including the possibility of zero
events. Uncovered portions of the physical page are irrelevant.

`EventOutputProperties::max_events_per_index` remains density/capacity metadata,
not a literal per-timestamp event quota. The semantic maximum for one page should
be based on the number of covered positions in that page, not blindly on full
physical page width:

```text
covered_positions = measure(page_domain)
required_event_slots = ceil(max_events_per_index * covered_positions)
```

Indexed execution is not realtime, so semantic maximum capacity does not require
preallocating that entire payload for every page. A practical implementation can
commit packed event storage dynamically, preferably from reusable slabs/arenas,
while enforcing the calculated maximum. Modest initial reservation or fixed
preallocation may be used if profiling shows that allocator variance harms UI
responsiveness; reserving every page to a conservative theoretical maximum is not
required by the design.

Rematerializing an invalid event page replaces its old event payload for the
covered page domain.

Because one logical indexed event read can span multiple independently stored
pages, the node-facing read API should expose a segmented ordered range/iterator
rather than promise one contiguous `std::span<TimedEvent>`. Iteration preserves
global timestamp order across page boundaries and never exposes events outside
input coverage.

## 16. Dynamic cache storage versus `NodeStorage`

One executable generation has one canonical fixed-layout `NodeStorage`, but
dynamically growing indexed cache pages are **not** part of that fixed layout.

`NodeStorage` contains storage whose shape is known when the `CompiledGraph` is
built, including:

- sequential `State`;
- indexed-domain persistent node state (`IndexedState`, currently
  `CompiledState`);
- compiler-owned bounded persistent regions;
- bounded reusable workspaces where useful; and
- realtime carry/history/feedback storage selected for persistent placement.

Executor-managed indexed output caches are a generation-local dynamic sidecar.
Conceptually:

```cpp
struct IndexedOutputRuntime {
    IndexedCoverage coverage;
    IndexedCachePolicy policy;
    PageDirectory pages;
};

struct IndexedGenerationRuntime {
    std::vector<IndexedOutputRuntime> outputs;
    IndexedTransactionWorkspace workspace;
};
```

The exact representation may differ, but dynamic page count/access history must
not force a second fixed `NodeLayout` or make `NodeStorage` variable-sized.

An indexed source may already own authoritative data in `IndexedState` or another
stable backing resource. Lowering may expose that data through a direct indexed
representation instead of duplicating it into executor cache pages. The three
physical cases are therefore:

```text
authoritative/direct node storage
transaction-local materialization
retained executor cache pages
```

All implement the same indexed semantics.

## 17. Indexed transaction workspace

Indexed access planning/evaluation needs request-sized temporary storage that is
not necessarily bounded at graph compile time. `GraphExecutor` should own/reuse a
transaction workspace or arena containing things such as:

- forward/reverse region-set work buffers;
- selected-page/materialization plans;
- transaction-local `cache = never` sample/event values;
- temporary event payloads and segmented views; and
- temporary pins/references to retained pages.

Transaction-local values remain shareable across all consumers in that logical
transaction. Once their last consumer is complete, storage may be reused by later
work in the same transaction; future liveness packing is an implementation
optimization.

Bounded scratch with a useful compile-time maximum may still be declared into
`NodeStorage`. Request-sized arbitrary cache/query storage belongs in the dynamic
executor sidecar/workspace.

## 18. Indexed connected components

The **indexed subgraph** is the project graph restricted to indexed edges and
their participating nodes.

An **indexed connected component** is a maximal weakly connected component of
that indexed subgraph. The implementation may use the shorter term **indexed
component** after this definition.

Lowering may precompute per component:

- requestable indexed output ordinals;
- reverse dependency order;
- forward change order;
- forward tock/evaluation order;
- fanout/convergence structure;
- constant node/port/state offsets and callback targets;
- trivial/no-op propagation operations; and
- bounded reusable planning workspace where useful.

Separate indexed components cannot share indexed dependency work and may be
processed independently.

If indexed cycles are eventually legal, strongly connected components are a
separate scheduling concept inside an indexed connected component; "component"
in this document does not mean SCC.

## 19. Initialization and initial coverage publication

Having `tock_region_batch()` does not imply that a node executes during project
initialization.

Initialization first constructs/migrates node state/resources. The generation
then performs an **initial forward transaction** that establishes indexed output
coverage from initialized state and upstream coverage. This transaction may call
forward-region logic with zero changed indexed-input regions. It does not by
itself materialize indexed output pages.

After initial coverage is known:

- UI-only indexed outputs may remain completely unmaterialized until first
  requested; and
- indexed data that must be available to realtime execution is materialized into
  a candidate indexed snapshot before that snapshot is published to the live
  graph.

Authoritative source data already present in node state/backing storage may be
immediately valid through its direct representation without a tock.

## 20. Executor-controlled mutation entry points

Any mutation that affects active indexed semantics must enter through an
executor-controlled boundary. Code outside `GraphExecutor` must not mutate
active indexed state and retained validity behind the executor's back.

Conceptually, UI/node-state mutation is:

```text
UI / project operation
        |
        v
executor-owned indexed mutation
        |
        v
modify/replace relevant node indexed state
        |
        v
create new desired indexed semantic version
        |
        v
run forward coverage/change transaction
```

The mutation may have zero changed indexed inputs and still produce output
coverage/value changes.

Realtime-originated mutations use a different handoff because the realtime path
must not allocate, lock dynamic page structures, or run indexed graph traversal.
A recorder-like `tick_block()` may write bounded authoritative state and append a
bounded change notification to pass-local/preallocated storage. After the whole
live graph block finishes, `GraphExecutor` consumes those notifications and
starts the corresponding forward transaction outside the hot path.

## 21. Indexed semantic versions and stale-work rejection

Indexed computation may overlap realtime processing and may also be superseded by
new edits before it finishes. Therefore retained validity is always relative to
an indexed **semantic version**.

A tock/evaluation transaction is tagged with the target version against which its
coverage/state/dependencies were planned. Results may only be committed to that
same candidate version. If a newer edit supersedes the candidate, old work may be
cancelled or allowed to finish, but it must not make the newer version valid by
accident.

The first implementation may simply discard superseded results. Cross-version
page reuse is a later optimization when the executor can prove that the page's
dependencies and coverage semantics are unchanged.

Within one candidate version, mutation/forward planning and commit ordering must
be serialized enough that a tock cannot race a state change and incorrectly mark
an old result valid for the new state. Epoch/version checks at commit are the
minimum correctness mechanism if evaluation runs concurrently.

## 22. Realtime observes immutable published indexed snapshots

Realtime execution never waits for indexed recomputation and never triggers
`tock_region_batch()` on the audio thread.

If an edit changes indexed data used by live DSP, the current live graph keeps
using the previously published complete indexed snapshot while the new candidate
is evaluated as quickly as possible off the realtime path.

Conceptually:

```text
live block N uses published indexed snapshot 41

UI/state edit
    -> desired indexed version 42
    -> exact forward invalidation/coverage update
    -> reverse demand + tock for live-required pages of 42

live blocks N+1, N+2, ... continue using snapshot 41

version 42 live-required indexed state becomes complete
    -> candidate snapshot 42 is ready

---- whole-live-graph block boundary ----

publish snapshot 42 atomically

next live block observes snapshot 42 everywhere
```

Publication occurs only **before or after execution of the entire live graph
block**, never between node ticks. One live graph pass therefore observes exactly
one coherent indexed snapshot.

Pages/representations visible through a published snapshot are immutable for the
lifetime of any live pass that can observe them. A candidate version writes new
page/direct representations rather than mutating published live-visible payload
in place. Unchanged pages may be structurally shared between snapshots.

The old snapshot is reclaimed only after no live pass can still reference it.
Because publication/reclamation happens at known whole-graph boundaries, the
implementation can use a lightweight generation/epoch scheme rather than
general-purpose per-read locking.

This intentionally chooses temporary semantic latency after an edit over risking
a realtime underrun.

## 23. Live-required indexed demand

The executor must know which indexed data a live snapshot requires before that
snapshot is publishable. `tick_block()` may never discover an invalid indexed
page and synchronously demand it.

A conservative first implementation may require all covered pages of an indexed
input that is readable by realtime execution. A later node trait can describe a
narrower stable live-read requirement where full coverage would be excessive.

Whichever authored mechanism is chosen, publication readiness is explicit:

> A candidate indexed snapshot is live-publishable only when every indexed page
> that realtime execution may read from that snapshot is complete and valid for
> that candidate version.

UI-only pages need not be materialized merely to publish a live snapshot.

## 24. Realtime recorder/source nodes

Realtime-to-indexed conversion is explicit. There is no implicit recording edge.

A recorder-like node owns the semantics by which realtime input is associated
with global indexed positions. During realtime execution it may write bounded
authoritative source data into indexed persistent state, update/queue coverage
changes, and report exact changed indexed output regions through the realtime-to-
executor notification handoff.

That report does not mean the recorder's newly authored source data is stale. It
means downstream derived indexed outputs may have changed and therefore create a
new indexed semantic version whose retained derived pages are invalidated by
forward propagation.

Downstream tocks remain demand-driven. Realtime playback does not turn the
indexed graph into a second audio-rate scheduler.

## 25. Indexed event/sample reads from node callbacks

Sample indexed reads address global covered sample indices. Since retained pages
are whole-page-valid, individual sample access is a simple read from a valid
selected page/direct representation.

Indexed event reads may span multiple event pages and therefore use a segmented
ordered iterator/range rather than requiring contiguous storage. The iterator
may hide page transitions but must preserve timestamp order and must never expose
or request outside input coverage.

Both tick and tock accessors expose input coverage. Debug validation should catch
attempted indexed reads outside coverage and attempted live reads from a page not
present in the published live snapshot.

## 26. External indexed-access result lifetime

Application/UI callers should not receive unpinned raw pointers into evictable
executor pages.

Two valid host-side result models are:

- return owned/copy results; or
- return an explicitly scoped/pinned view that keeps every referenced page/direct
  representation alive and non-evictable for the view lifetime.

The simplest initial application boundary should prefer owned results. Generated
internal tock/reverse execution may of course use borrowed transaction/cache
views whose lifetime is controlled by `GraphExecutor`.

## 27. Outward change notification

After a forward transaction finishes updating internal coverage/change/validity
metadata, the executor may publish a coalesced indexed-output change notification
for subscribed external consumers.

Conceptually it contains:

```text
output identity
indexed semantic version/revision
new coverage (or a coverage-changed indication)
exact changed IndexedCoverage
```

Notification is emitted after the internal forward transaction, not recursively
from individual propagation callbacks. Presentation/UI code can react by issuing
ordinary indexed access requests; the notification itself does not force tock
execution.

## 28. Generation replacement and cache migration

A new `CompiledGraph` executable generation receives a new generation-local
indexed runtime sidecar.

For the initial implementation:

- compatible `State` / indexed persistent node state migrates through ordinary
  `NodeStorage` lifecycle rules; but
- retained executor cache pages do **not** migrate between executable
  generations.

That is conservative and keeps cache reuse outside correctness. Later cache-page
migration may be added only with a proof of stable node/output identity,
compatible coverage semantics, equivalent indexed implementation/dependencies,
and compatible payload representation.

Old and new executable generations and old/new published indexed snapshots may
coexist briefly while a live block finishes and safe-boundary activation occurs.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output realtime root. It
does not gain synthetic indexed output ports or a project-wide tock callback.

`CompiledGraph` (the JIT artifact) carries immutable metadata mapping internal
indexed output endpoints to generated indexed-component executors. The name
`CompiledGraph` remains appropriate because this object is generated machine
code plus immutable compilation metadata; it is unrelated to the old compiled
port terminology.

Static indexed topology is specialized during GraphJit lowering. Runtime
transactions manipulate exact coverage/change/demand sets, page-validity/version
state, dynamic page directories, and generated component entrypoints rather than
rebuilding graph adjacency or topological orders for each request.

`GraphExecutor` owns:

- canonical fixed-layout `NodeStorage` for each retained executable generation;
- dynamic indexed generation sidecars/page directories;
- indexed semantic versions/candidate snapshots;
- transaction workspaces;
- forward mutation/change transactions;
- reverse demand/tock transactions;
- live-required indexed readiness;
- whole-live-block snapshot publication/reclamation; and
- external indexed access/change-notification lifetime.

## 30. Deliberately open implementation/tuning choices

The semantic model above does not require early commitment on several tuning or
ABI details. Leave these open until implementation/profiling provides evidence:

- exact cache page width and whether sample/event outputs use the same default;
- dense versus coverage-packed sample payload thresholds;
- the concrete stable page arena/chunk allocator and sorted/flat directory type;
- event-page inline/reserved capacity, slab sizes, and whether UI workloads merit
  more aggressive preallocation;
- exact `automatic` cache-retention/eviction heuristics and memory budgets;
- whether `always` pages have an emergency memory-pressure escape hatch;
- the exact node trait/API used to narrow live indexed read requirements below
  conservative full input coverage;
- the concrete executor mutation/change-notification queue ABI;
- cancellation versus discard policy for superseded candidate tocks;
- when cross-version or cross-executable-generation page reuse becomes worth
  proving; and
- whether application-facing indexed results remain owned copies or gain an
  explicit pinned-view API for high-volume visualization paths.

These choices may change physical cost and responsiveness but must preserve exact
coverage semantics, whole-page validity, semantic-version correctness, and
whole-live-block snapshot publication.

## 31. Static validation

Registered node declarations should validate the indexed contract at the node
boundary. At minimum:

- an indexed output requires a valid tock implementation unless its authoritative
  representation is explicitly handled by another supported source mechanism;
- at most one authored form of each optional unbatched/batched callback family is
  provided;
- callback signatures are valid;
- missing reverse propagation has a correct conservative covered-input fallback;
- missing forward propagation has a correct conservative coverage/change
  fallback for indexed input or local-state changes;
- indexed sample/event declarations and cache policies are structurally valid;
- `max_events_per_index` is finite/nonnegative where applicable;
- realtime/indexed state lifecycle callbacks are usable by canonical
  `NodeLayout`/`NodeStorage`; and
- node-facing indexed reads can be validated against input coverage/live snapshot
  availability in appropriate builds.

Diagnostics should name the node type, offending callback/port, and expected
alternative whenever practical.

## 32. Summary invariants

1. `indexed` describes globally addressed, order-independent sample/event data;
   it does not mean JIT compilation or a storage class.
2. `IndexedCoverage` is the sole semantic domain boundary; there is no indexed
   extent/bounding-hull abstraction.
3. Coverage is a canonical union of sorted, nonempty, disjoint, non-adjacent
   half-open regions.
4. Node callbacks never request indexed values outside input coverage.
5. Indexed input coverage is the union of connected/mapped indexed output
   coverages and is visible to both tick and tock code.
6. Cache pages are canonically aligned, but page boundaries never widen coverage.
7. One cache page is either wholly valid or wholly invalid for its exact
   `page_interval & coverage` domain and indexed semantic version.
8. Sparse logical/UI access selects pages; it does not require sparse per-sample
   retained validity or sparse per-sample tock processing.
9. Touching any invalid page causes its entire covered page domain to be selected
   for materialization.
10. Forward change propagation remains exact and is never widened to page
    boundaries merely because a page becomes invalid.
11. Reverse demand is exact until it touches an invalid upstream page; that page
    is then promoted to its entire covered page domain before reverse propagation
    through the producer continues.
12. `tock_region_batch()` receives only selected covered page domains, never
    uncovered page portions, already-valid pages, or blind sparse caller points.
13. Arbitrary node-local mutations may trigger forward processing with zero
    changed indexed inputs and may change output coverage and/or values.
14. Added or removed coverage is itself a semantic change propagated downstream.
15. Forward propagation never forces tock evaluation and continues through
    uncached intermediates.
16. Forward and reverse propagation are opposite graph-direction dependency
    queries, not mathematical inverses.
17. Cache policy is physical (`automatic` / `always` / `never`) and never changes
    indexed semantics; transaction-local sharing remains allowed for `never`.
18. Sample payloads may be dense or coverage-packed; this choice is independent
    of whole-page validity.
19. Event pages use whole-page validity plus packed ordered events, with semantic
    capacity based on `max_events_per_index * covered_positions`.
20. Indexed event reads may span pages and therefore use segmented ordered
    iteration rather than requiring contiguous storage.
21. Fixed node/indexed persistent state remains in canonical `NodeStorage`;
    dynamically growing indexed caches live in an executor-owned generation
    sidecar.
22. Indexed transactions may use reusable dynamic workspace/arena storage;
    request-sized temporaries need not live in `NodeStorage`.
23. Initial generation setup publishes coverage through a forward transaction
    before indexed demand is serviced.
24. All mutations affecting indexed semantics enter through an executor-owned
    boundary; realtime sources report bounded changes for later executor-side
    propagation.
25. Indexed validity/results are versioned. Work computed for an obsolete
    semantic version cannot commit as valid for a newer version.
26. Realtime execution observes one immutable published indexed snapshot for an
    entire live graph block and never waits for or triggers tock processing.
27. A newer live-required indexed snapshot publishes atomically only at a
    whole-live-graph block boundary after all pages realtime may read are ready.
28. Old published snapshots remain usable while a candidate computes; unchanged
    pages may be structurally shared and old snapshots are reclaimed after no live
    pass can reference them.
29. Retained executor cache pages do not migrate between executable generations in
    the initial implementation; cache migration is a later optimization.
30. The indexed subgraph is partitioned into indexed connected components for
    static planning and runtime batching.

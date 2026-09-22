# Indexed DSP ports and incremental indexed evaluation

This document is the normative design for **indexed DSP ports**, their
incremental evaluation model, and the executor-side storage/publication rules
needed to make indexed data usable by both UI-style random access and realtime
processing.

`indexed` replaces the older `compiled` DSP-port terminology. `compiled` is now
reserved for actual program/JIT compilation (`GraphJit`, `CompiledGraph`, LLVM
modules, generated code, and similar concepts). The public port declarations,
callback contexts, node traits, persistent indexed state, compiler records, and
GraphJit implementation metadata use indexed terminology directly; the former
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

> Indexed dependency propagation is exact in region space, while retained cache
> validity is page-granular. A cache page is either valid or invalid as a whole,
> but its semantic page domain is exactly `page_interval & output_coverage`; page
> boundaries never force node code to process outside real coverage.

> Forward propagation records semantic change and invalidates retained results
> without forcing evaluation. Reverse propagation starts from demanded coverage.
> At `cache = true` boundaries, missing demand is promoted to invalid/nonresident
> covered page domains; through `cache = false` outputs it remains exact.
> `tock_coverage()` materializes only that resulting required coverage.

> Realtime indexed access depends on the output's `cache` contract. `cache = false`
> permits realtime-compatible `tock_coverage()` execution inline on the audio
> thread and owns no indexed cache pages. `cache = true` is a materialization
> boundary: realtime may read it only from already-prepared retained/direct data.
> Semantic-version publication still occurs only at whole-live-graph block
> boundaries so one live pass sees one coherent indexed state.

> Cached indexed outputs must not close a semantic dependency cycle back into
> their own node. After whole-project semantic SCC detection, every connection
> sourced from a `cache = true` indexed output must strictly leave the semantic
> SCC containing that output's node. The node itself may participate in an SCC;
> only its cached indexed outputs are forbidden from feeding that SCC.

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

## 5. Sparse logical requests and cached page materialization

External indexed sample queries may be sparse. UI waveform rendering may ask for
one sample per display frame, for example. How that sparse demand is materialized
depends on the output's boolean cache contract.

For `cache = true`, sparse query positions do **not** require sparse per-sample
validity or processing. For every requested covered sample/event region, the
executor identifies touched cache pages. If a touched page is already valid for
the target semantic version, that page is a cache hit. If it is invalid or absent,
the required materialized work for that output is the page's **entire page
domain**:

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

The uncovered gap is never processed, but the whole covered domain of the invalid
touched cached page is materialized atomically.

For coverage spanning several pages:

```text
coverage = { [10,3000) }
```

its cached page domains are:

```text
page 0: { [10,1024) }
page 1: { [1024,2048) }
page 2: { [2048,3000) }
```

A cached request for sample `500` only touches/materializes page zero.

This gives useful cached behavior at all UI zoom levels:

- zoomed in, many requested samples touch many consecutive pages, so evaluation
  becomes naturally dense/sequential;
- around one requested sample per page, each touched page is computed once; and
- zoomed far out, requested samples land in widely separated pages, so only sparse
  pages are touched.

For `cache = false`, there are no output pages to select. The UI request remains
exact after intersection with coverage and `tock_coverage()` writes directly into
the request/result or transaction storage. Any upstream `cache = true` dependency
may still page-expand when reverse planning reaches that cached boundary.

Page width is therefore an implementation tuning knob only for cached outputs,
trading bounded recomputation against directory/payload overhead. Pages are
expected to be small enough that recomputing one complete page domain after a tiny
invalidation is acceptable.

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
        propagate_reverse_coverage(producer)
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

A node may therefore run its forward-coverage logic with **zero changed indexed
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

No `tock_coverage()` call is implied by forward processing. Many edits may
accumulate/supersede invalidations before any indexed computation is demanded.

## 8. `propagate_forward_coverage()`

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

## 9. `propagate_reverse_coverage()`

The reverse dependency callback answers:

> Given exact covered regions of this node's indexed outputs that must be
> materialized, which covered regions of its indexed inputs must be available to
> compute them?

Conceptually:

```text
R(node): materialized output coverage sets -> required input coverage sets
```

It runs in reverse graph direction. Requirements reaching a node through multiple
downstream paths are accumulated/unioned before the node is visited.

The requirement seen at a particular output depends on that output's cache
contract:

- for `cache = false`, the requirement remains the exact demanded covered region;
  there is no page boundary to promote it to;
- for `cache = true`, a valid touched page satisfies that part of the requirement,
  while an invalid/nonresident touched page promotes the requirement to the
  complete `page_interval & output_coverage` domain before reverse propagation
  continues through that producer.

After the callback reports required input regions, every requirement is clipped to
that input's exact coverage. The same cached/uncached rule is then applied at each
upstream output. Thus page granularity enters dependency planning only at cached
materialization boundaries; uncached indexed chains may remain exact throughout.

The callback is optional. A conservative synthesized behavior may require all
covered regions of each indexed input. Output-only indexed sources have nothing
upstream to request and therefore require no reverse callback.

For an output declared `cache = false` that can participate in a realtime pull
path, any reverse mapping that must execute dynamically on the audio thread is part
of the realtime-safety contract. GraphJit should specialize or precompute such
mappings wherever possible rather than invoking a generic dynamic planner from the
hot path.

## 10. `tock_coverage()`

`tock_coverage()` is the one-node indexed evaluation callback. Its context carries
requested `IndexedCoverage` per indexed output; one invocation may therefore
compute many disjoint regions and multiple output ports for the same node.

The callback receives only work that actually needs computation for the target
semantic version:

- a `cache = true` output receives the union of selected invalid/nonresident
  pages' exact **covered page domains**;
- a `cache = false` output receives the exact demanded covered regions, because no
  persistent indexed page representation exists for that output.

It never receives uncovered portions of a physical page. For cached outputs it
also never receives already-valid pages or the caller's isolated sparse sample
positions after those positions selected whole cached pages.

The callback:

- sees indexed inputs and indexed outputs;
- operates on covered global sample/event regions;
- cannot request indexed inputs outside their published coverage;
- cannot depend on realtime-only inputs or outputs;
- cannot depend on sequential realtime `State`;
- may use persistent indexed-domain state/workspaces; and
- must produce observable results independent of indexed request order.

For `cache = true`, successful evaluation commits each selected page atomically
valid for the target semantic version. There is no partially valid retained page.
For event outputs, rematerializing a page domain replaces the old cached event
contents for that page domain; stale events must not survive when a new evaluation
emits fewer events. Within one output invocation, node code emits events in
nondecreasing global timestamp order; lowering/storage must not add a mandatory
release-time sorting pass to repair unordered producer output.

For `cache = false`, no indexed cache page is allocated or committed. The callback
writes into storage owned by the current consumer/evaluation plan: for example a
UI result buffer, transaction-local intermediate storage, compiler-owned live
transient storage, or a directly bound realtime input representation. Multiple
consumers in the same transaction/pass may still share one materialization when
lowering/liveness permits.

The name deliberately does not contain `batch`: this is still one node. A future
`tock_coverage_batch()` may evaluate multiple nodes simultaneously, with separate
coverage and indexed state for each node.

## 11. Indexed access transaction

A logical indexed access may request outputs from any number of internal nodes.
Requests are grouped by indexed connected component and coalesced within each
component.

The planner treats cached and uncached outputs differently.

For a requested `cache = false` sink output:

1. intersect the request with exact output coverage;
2. keep that covered demand exact; there are no output cache pages to test or
   promote; and
3. reverse-propagate the exact demand until it either reaches an indexed source or
   crosses a `cache = true` boundary.

For a requested `cache = true` sink output:

1. intersect the request with exact output coverage;
2. determine which canonical physical pages that covered request touches;
3. satisfy touched pages already resident/valid for the target semantic version;
   and
4. promote every missing/invalid touched page to its complete
   `page_interval & coverage` domain before reverse propagation through that
   producer.

The same rule applies recursively at every upstream indexed output. A cached valid
page terminates that branch. A cached missing/invalid page promotes demand to its
whole covered page domain. An uncached output preserves exact demand and continues
reverse traversal without creating a page.

Conceptually:

```text
requested sink positions/regions
        |
        v
intersect exact sink coverage
        |
        v
inspect sink cache contract
        |
        +-- cache=false -> exact covered demand --------------------+
        |                                                           |
        `-- cache=true -> touched pages                             |
                |                                                   |
                +-- valid/resident pages -> satisfied               |
                |                                                   |
                `-- missing/invalid pages -> full covered domains --+
                                                                    |
                                                                    v
reverse planning in precomputed reverse order
        |   union/coalesce requirements at convergence points
        |   clip requirements to exact input coverage
        |   apply the same cached/uncached boundary rule upstream
        v
complete dependency/materialization plan
        |
        v
forward evaluation in dependency order
        |   cache=false -> direct/transaction/transient destination
        |   cache=true  -> retained page payload
        v
tock_coverage() for each implicated node's required coverage
        |
        v
commit cached pages / return or forward uncached materializations
        |
        v
return requested sink values/events
```

A fully cached request can therefore be satisfied without reverse propagation or
tock execution. A fully uncached request may traverse and execute an indexed chain
without allocating any persistent indexed pages at all.

Sparse UI fetches do not require sparse per-sample processing at cached outputs:
requesting one point in an invalid cached page selects that page's entire exact
covered domain. At uncached outputs, however, there is no reason to page-expand a
sparse request; the exact requested coverage may be computed directly.

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
propagate_forward_coverage()
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

## 13. `cache` is a scheduling/materialization contract

Indexed output caching is a boolean declaration, not an `automatic` / `always` /
`never` retention preference:

```cpp
struct IndexedOutputConfig {
    bool cache = true;
};
```

The boolean chooses a legal scheduling model, not merely a retention preference.
Some indexed producers are cheap, bounded deterministic functions for which
pre-materializing pages would add avoidable residency, allocation/bookkeeping,
and copy overhead: those outputs may declare `cache = false` and materialize
directly into their current consumer, including during realtime execution. Other
indexed producers may be expensive or have execution cost that cannot be relied
upon to meet an audio deadline: those outputs declare `cache = true`, so realtime
may consume them only through already-prepared materialization. Neither strategy
is universally preferable; `cache` tells GraphJit where indexed computation may
legally occur.

The default should be `true`. Declaring `cache = false` is a positive performance
contract by the node author; accidentally omitting a declaration must not make an
arbitrary indexed callback eligible to run on the audio thread.

### `cache = false`

`cache = false` means all of the following:

- the output owns **no persistent indexed cache pages**;
- `tock_coverage()` for this output is expected to be suitable for realtime
  execution when the output participates in an indexed-to-realtime pull path;
- any dynamically required reverse-coverage mapping on that path must likewise be
  realtime-compatible, though GraphJit should specialize/precompute it whenever
  possible;
- non-realtime/UI requests materialize directly into caller/result or
  transaction-local storage;
- realtime materialization writes into compiler-planned direct or transient live
  storage rather than round-tripping through a dynamic indexed page; and
- one materialization may still be shared by fanout/consumers during the current
  transaction/pass.

Realtime-compatible here means the same kind of bounded hot-path contract as
`tick_block()`: no blocking on non-realtime work, no unbounded/dynamic allocation,
no unsafe locks, and execution cost expected to keep up with realtime use. This is
an authored contract rather than something the type system can prove.

Cheap deterministic function-of-index/CBRNG outputs are the canonical example.
A recorder or memory-backed source with an authoritative direct representation may
also need no additional executor page cache.

### `cache = true`

`cache = true` means the output is a persistent materialization boundary:

- missing data is materialized into canonical retained pages;
- `tock_coverage()` for that output is **not** permitted as a fallback from the
  realtime thread;
- realtime consumers may read only already-prepared valid/resident data (or a
  supported authoritative direct representation with equivalent readiness);
- UI/non-realtime requests may create/reuse pages on demand; and
- pages may later be evicted when they are unpinned and can be regenerated.

`cache = true` does **not** mean every covered page is eagerly generated, retained
forever, or immune to memory-pressure eviction. Caching policy after declaration
belongs to `GraphExecutor`: page residency, eviction, prefetch/lookahead, and
prediction may evolve without changing node-facing semantics.

The boolean therefore answers a stronger and simpler question:

> Is this indexed output a retained materialization boundary that may be too
> expensive/unbounded for synchronous realtime generation, or is it guaranteed
> suitable for uncached inline materialization?

Correctness must not depend on a sophisticated cache predictor. The first
implementation should use simple on-demand UI materialization plus a conservative
fixed realtime lookahead for cached outputs, with only straightforward eviction of
unpinned pages where needed. More advanced policies (adaptive lookahead, LRU/clock,
transport/seek prediction, timeline-click speculation, measured tock cost) are
optional later optimizations.

The scheduling promise also constrains feedback topology. A cached indexed output
may be produced by a node that participates in realtime feedback, but that output
must not feed any dependency path that returns to an input of the same node. Such
a path would let the realtime SCC consume cached data and then invalidate the same
cached producer through the cycle, forcing the cached value to advance at realtime
pace even though `cache = true` explicitly declines that guarantee. Section 18
defines the whole-project SCC validation precisely.

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

One `tock_coverage()` may cover many selected pages. Page boundaries must not
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

## 16. Dynamic cache storage, stable output identity, and `NodeStorage`

Each executable generation still has one canonical fixed-layout `NodeStorage`,
but dynamically growing indexed cache pages are **not** part of that fixed
layout and should not be owned merely by one JIT generation when the producing
indexed output has stable project identity.

`NodeStorage` contains storage whose shape is known when the `CompiledGraph` is
built, including:

- sequential `State`;
- indexed-domain persistent node state (`IndexedState`);
- compiler-owned bounded persistent regions;
- bounded reusable workspaces where useful; and
- realtime carry/history/feedback storage selected for persistent placement.

`GraphExecutor` instead owns a stable indexed-cache store plus per-generation
bindings. Conceptually:

```cpp
struct StableIndexedOutputId {
    StableConcreteNodeId node;
    IndexedOutputPortId output;
};

struct IndexedCacheEntry {
    StableIndexedOutputId id;
    // Exists only for cache=true outputs.
    // Versioned coverage/page state and stable page payload storage.
};

struct IndexedGenerationRuntime {
    std::vector<IndexedOutputBinding> outputs;
    IndexedTransactionWorkspace workspace;
};
```

The exact C++ representation is deliberately open, but the ownership rule is
not:

- a concrete node that is reachable through stable virtual-node project identity
  and an ordered direct-member selector has stable concrete-node identity;
- a stable indexed output extends that concrete-node identity with the stable
  output-port identity/schema used by the graph;
- for `cache = true`, a new executable generation binds its generation-local
  indexed output ordinals to those stable cache entries; and
- a cached concrete node/output with no stable project identity receives
  generation-local cache storage and cannot carry retained cache validity across
  executable replacement. `cache = false` outputs have no cache entry to migrate
  or rebind.

In the current graph identity model, virtual nodes retain ordered
`node_bundle_handles`, and persistent project paths use semantic virtual-node
selectors plus `ProjectVirtualMemberSelector` ordinals rather than builder-local
handles. Indexed-cache identity should reuse that canonical project-path seam
rather than invent a second generation-local identity system.

Stable cache reuse is therefore normally **rebinding**, not copying or bulk
migration. Page directories and payload allocations can remain owned by the
executor cache store while old and new executable generations refer to the
appropriate semantic versions. If the implementation instead nests cache
objects under generation runtimes, carrying a stable cache forward should still
be an ownership/handle transfer rather than a payload copy.

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
- non-realtime `cache = false` result/intermediate sample/event values;
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
processed independently. "Indexed connected component" is still not a synonym
for SCC: it is a weak-connectivity partition used to share indexed planning work.

### Semantic SCC constraint for cached indexed outputs

Feedback validation uses a separate **whole-project semantic dependency graph**.
Its vertices are concrete nodes. Its directed edges represent logical data
dependencies of every access direction that can carry causality between nodes,
including realtime and indexed connections. A detached/feedback connection is
restored for semantic cycle membership even though detach removes its same-slice
tick dependency. This semantic graph is intentionally broader than the graph used
to topologically order one realtime slice.

Let `N` be a concrete node and `O` one of `N`'s indexed output ports with
`cache = true`. The required invariant is:

> No directed dependency path beginning with a connection sourced from `O` may
> eventually reach any input of `N`.

Because current node metadata does not describe input-to-output dependency at
finer granularity, validation conservatively treats the concrete node as the
dependency unit. After computing SCCs of the semantic dependency graph, the rule
has a simple equivalent form:

> Every direct consumer of a `cache = true` indexed output must belong to a
> different semantic SCC from the output's owning node.

If a cached output fans out, **every** branch must satisfy the rule. One branch
remaining inside the owner's SCC makes the graph invalid even when other branches
leave it. A direct cached indexed self-loop is therefore invalid as well.

The node itself is not forbidden from SCC membership. This is legal:

```text
          realtime feedback
        +-------------------+
        |                   |
        v                   |
       A -----------------> B
       |
       `-- indexed cache=true --> C
```

provided the cached output from `A` only reaches nodes outside `A`'s semantic SCC.
This lets a cyclic realtime node publish cached analysis/UI data downstream
without making that cache part of the feedback loop.

This is invalid:

```text
       A -- indexed cache=true --> B
       ^                           |
       |                           |
       `-------- dependency -------'
```

because the cached output can eventually affect an input of `A`. The SCC can
consume a cached value and then invalidate the producer on which that value
depends. Servicing the loop would therefore require cached indexed computation to
advance in lockstep with realtime execution, contradicting the `cache = true`
contract that its `tock_coverage()` need not be realtime-compatible. Ahead-of-time
prefetch cannot in general solve this because the state needed for a future
iteration may be created by the preceding iteration of the same cycle.

`cache = false` indexed outputs do not have this cached-self-dependency problem:
they own no retained page validity and promise realtime-compatible inline
materialization. They may therefore participate in an SCC when the surrounding
cycle is otherwise legal. This rule does **not** create a new indexed fixed-point
or feedback model, however. Cycles are still legalized only by the existing
explicit realtime feedback/detach semantics; `cache = false` is necessary for an
indexed output retained inside such an SCC, not sufficient to make an arbitrary
cycle valid.

GraphJit should perform this check after the complete project graph is assembled
and semantic SCC membership is known. Using only the same-slice/realtime execution
SCCs is insufficient because an indexed edge can itself be the edge that closes a
mixed realtime/indexed semantic cycle.

Until inline `cache = false` indexed work is actually supported inside live SCC
lowering, an implementation may conservatively reject a broader class such as all
SCCs crossed by indexed dependencies. That is a temporary capability gate, not
the final graph semantic constraint; it should eventually relax to the cached-
output rule above.

## 19. Node creation and coverage publication

Having `tock_coverage()` does not imply that a node executes merely because a
project or executable generation was created.

The primitive lifecycle event is **semantic concrete-node creation**: a node has
no stable retained counterpart and is being introduced into the indexed graph.
Project startup is simply the case where every project node is created. Producing
new machine code for a stable retained node is not by itself semantic node
creation.

Whenever a concrete node is created:

1. its ordinary node state/resources are initialized;
2. its current indexed-input coverages are made available once upstream coverage
   is known;
3. a forward-side invocation is scheduled with a node-created/state-present cause
   and may have zero changed indexed-input regions; and
4. the node publishes the exact coverage of each indexed output before consumers
   may issue indexed demand against those outputs.

For a newly created output, the old coverage is empty. Therefore all newly
published coverage is also an exact semantic output change:

```text
old coverage = {}
new coverage = C
added         = C
```

That added coverage propagates downstream through the ordinary forward-coverage
machinery. A chain of newly created indexed nodes therefore establishes coverage
in forward dependency order; there is no separate project-startup-only coverage
discovery system.

A retained stable node does not republish/recompute all coverage merely because a
new `CompiledGraph` generation was JIT-compiled. Its prior coverage/cache state is
reused unless node state, indexed connection sets, implementation semantics, or
another explicit forward cause requires recomputation.

UI-only indexed outputs may remain completely unmaterialized after coverage is
known. For realtime use, `cache = false` outputs need no prebuilt pages and may be
materialized inline by the generated live pull plan. `cache = true` boundaries
must have the pages needed by the imminent live blocks prepared before those
blocks consume them. Authoritative source data already present in node state or
stable backing storage may be immediately readable through a direct representation
without an executor page copy.

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

An indexed **connection-set change** is another executor-controlled forward
cause. Addition, removal, replacement, or another semantic change to the set of
connections feeding one indexed input conservatively marks that whole logical
input as changed. Let the executor retain both aggregate coverages around the
change:

```text
changed_input = old_input_coverage | new_input_coverage
```

The node's forward callback sees the new current input coverage plus that changed
region set. This intentionally may overinvalidate unaffected portions of a fan-in
input; the rule is simple, correct, and keeps connection-combination semantics out
of generation reconciliation. The node's own forward mapping may narrow the
resulting output change. Connection-set changes never imply unbounded invalidation
because indexed input coverage is finite.

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

The first implementation may simply discard results produced by work that was
already superseded while executing. Pages that were never invalidated between
semantic versions may be structurally shared by construction; reusing the output
of **superseded in-flight work** in a newer version is a separate optimization
that requires an equivalence proof.

Within one candidate version, mutation/forward planning and commit ordering must
be serialized enough that a tock cannot race a state change and incorrectly mark
an old result valid for the new state. Epoch/version checks at commit are the
minimum correctness mechanism if evaluation runs concurrently.

## 22. Realtime indexed reads and immutable published semantic snapshots

Indexed-to-realtime connections are lowered according to the indexed producer's
`cache` contract. The audio thread may execute `tock_coverage()` only through a
`cache = false` live pull path. It must never use inline tock as a fallback for a
missing `cache = true` page.

For one realtime input connected to indexed data, the current live block is
projected against the indexed input's published coverage:

- inside coverage, values/events come from prepared cached/direct data or from an
  inline `cache = false` tock path;
- outside coverage, the realtime input yields its declared `neutral_value` (or no
  events) without allocating or materializing indexed storage; and
- inside coverage, a missing required `cache = true` page is a readiness/underrun
  failure, **not** permission to substitute `neutral_value` and not permission to
  run that cached producer's tock on the audio thread.

### Live pull lowering through uncached outputs

GraphJit should lower indexed-to-realtime access as a pull plan rooted at each
realtime consumer. Walking backward through indexed dependencies:

```text
realtime input
      ^
      |
cache=false indexed output  -> inline/realtime-compatible tock
      ^
      |
cache=false indexed output  -> inline/realtime-compatible tock
      ^
      |
cache=true indexed output   -> prepared page/direct materialization boundary
```

A `cache = false` chain may therefore execute synchronously within the live graph
block while reading prepared `cache = true` boundaries upstream. No dynamic page
storage is created for the uncached outputs.

The realtime storage planner should exploit the same placement opportunities as
ordinary realtime transport. In particular, an uncached indexed producer may
write directly into a realtime input's data representation when there is one
effective consumer, representation is compatible, and the input has no history or
other lifetime requirement that forces separate storage. Otherwise GraphJit uses
compiler-owned transient/pass-local storage, with ordinary fanout/liveness reuse.
The implementation should avoid a page allocation -> copy -> input-buffer roundtrip
when direct or transient placement can satisfy the connection.

The same principle applies to events: an uncached indexed event producer writes
into bounded live event storage sized/planned for the current live use rather than
an executor cache page.

### Published semantic versions

Node state, coverage, connection semantics, and cached-page bindings observed by
realtime still belong to one immutable **published indexed semantic version** for
the entire live graph block. A UI/state edit may build a candidate semantic
version while the current live graph continues using the previous published one.

For `cache = false` outputs, publication does not wait for pages that do not exist;
the new published state/coverage is enough for their next inline tock. For every
`cache = true` boundary that the new live pull plan can reach near the current
transport position, the candidate must first have the immediately required pages
prepared.

Conceptually:

```text
live block N uses published semantic version 41

UI/state edit
    -> desired semantic version 42
    -> exact forward invalidation/coverage update
    -> prepare cache=true pages needed by the imminent live window of 42
       (cache=false paths require no page precomputation)

live blocks N+1, N+2, ... may continue using version 41 while 42 is not ready

version 42 reaches the required live-readiness threshold

---- whole-live-graph block boundary ----

publish semantic version 42 atomically

next live block observes version 42 everywhere
```

Publication occurs only **before or after execution of the entire live graph
block**, never between node ticks. One pass therefore sees one coherent semantic
version and one coherent set of published cached/direct page references.

Pages/direct representations visible through a published live view are immutable
for the lifetime of any pass that can observe them. Candidate cached pages are
written separately; unchanged pages may be structurally shared. Old state/page
views are reclaimed only after no live pass can reference them, permitting a
lightweight generation/epoch reclamation scheme.

Preparing additional future pages for the **same** semantic version does not need
to create another semantic version. Page-residency/materialization snapshots may
advance at whole-live-block boundaries while node/connection semantics remain
unchanged.

## 23. Realtime readiness, lookahead, and cached-page eviction

There are two primary sources of indexed demand:

1. UI/presentation requests for sparse pages/regions from a small number of
   concurrently displayed node lanes; and
2. realtime pull paths, which need consecutive upcoming audio blocks available.

They use the same indexed dependency/evaluation machinery but with different
request shapes.

### `cache = true` live boundaries

A cached output on a live pull path requires ahead-of-time residency. The executor
maintains a ready window around/ahead of the current transport position, clipped
to exact output coverage. Only pages whose `page_interval & coverage` is nonempty
need storage. Realtime reads outside coverage receive the input `neutral_value`
without creating default-filled pages.

The initial implementation should remain simple:

- use a fixed configured/preselected number of pages or time span ahead of the
  playhead;
- on playback start or explicit seek, prioritize the target page and consecutive
  pages ahead before depending on that location;
- continue advancing the ready frontier while playback progresses;
- pin pages visible to the current published live view;
- allow only unpinned pages to be evicted; and
- use a simple bounded-memory eviction policy if needed rather than predictive
  heuristics.

Sophisticated prediction is deliberately optional. A future executor may use
transport direction, loops, measured tock cost, visible UI lanes, timeline-click
hints, LRU/clock history, or adaptive lookahead to decide what to precompute and
what to evict. None of those policies change the `cache` contract.

`cache = true` does not guarantee that generation throughput exceeds playback. If
a producer fundamentally cannot prepare future pages fast enough, a finite
lookahead will eventually be exhausted. That is an indexed-readiness underrun;
the audio thread must not repair it by synchronously executing the cached tock or
by silently treating covered missing data as neutral. Playback/pre-roll policy and
underrun reporting are executor/application concerns layered above this contract.

### `cache = false` live outputs

An uncached output has no ready-page frontier and no eviction state. Its live
demand is the current required block/coverage, and GraphJit executes the needed
`tock_coverage()` work inline. Reverse traversal may continue through other
uncached outputs and stops at prepared cached/direct boundaries.

This is precisely why ahead-of-time caching would be wasteful for cheap outputs:
the current block can be generated directly into the final live representation,
then discarded/reused immediately after its consumers finish.

UI-only cached pages need not be materialized merely to make a semantic version
live-publishable, and UI materialization does not imply that every intermediate
uncached indexed output acquires persistent storage.

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

### External version selection and incomplete candidate results

Every externally returned indexed result is associated with the indexed semantic
version/revision from which it was read. An application/UI request for a newer
semantic version may cause ordinary reverse planning and tock work for pages that
are not yet valid in that version. Until those pages are complete, they are
**pending/not ready**; they must never be represented to the caller as empty
samples, zero events, default-valued data, or an artificial empty coverage merely
because candidate materialization has not finished.

If presentation code already holds a completed result from an older semantic
version, it may keep displaying that result while the requested newer-version
result is pending. Once the newer requested pages are complete, presentation can
replace the older result. Whether the UI visually marks the retained result as
stale/pending is presentation policy, but the indexed executor must not force a
blank intermediate state.

The host API may initially satisfy a requested semantic version synchronously, or
it may expose an explicit pending/not-ready result or future-like operation. The
exact ABI is implementation work. In either case, a caller must be able to tell
which semantic version produced a completed result and must not accidentally
combine independently returned data from different versions as though it were one
coherent indexed result.

A pure executable-generation replacement that rebinds an unchanged stable indexed
output does not create such a pending state: its existing valid cache pages remain
valid and immediately available.

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

## 28. Executable-generation reconciliation and stable cache rebinding

JIT compilation and indexed semantic invalidation are separate events. Producing
a new `CompiledGraph` generation does **not** by itself make stable indexed output
caches stale.

When `GraphExecutor` receives a candidate executable generation it reconciles the
old and new graph descriptions using stable concrete-node/output identities. The
new generation's local indexed endpoint ordinals bind to the existing stable
cache entries whenever those identities survive. Retained page directories and
payload allocations are therefore reused by reference/ownership, not copied.

Conceptually, reconciliation classifies indexed structure as:

```text
retained stable node/output
new semantic node/output
removed node/output
indexed input connection-set changed
node state changed
implementation/schema semantics changed
anonymous generation-local node/output
```

The important cases are:

- **retained stable output, unchanged indexed semantics:** rebind the new
  generation to the existing cache entry and preserve coverage, valid pages, and
  payloads;
- **retained stable node with an indexed connection-set change:** retain cache
  storage, compute the old/new aggregate coverage for each changed input, seed
  `old_input_coverage | new_input_coverage` as the whole-input changed region,
  and run ordinary forward propagation;
- **retained stable node with a local state mutation:** retain cache storage and
  use the ordinary node-local forward cause to update coverage/changed regions;
- **retained stable identity but incompatible implementation/output semantics:**
  retain compatible backing allocation if useful, but conservatively make the
  candidate output invalid over the affected coverage unless a stronger semantic
  compatibility proof exists;
- **new node:** initialize its node state and establish indexed output coverage
  through the node-creation rule in section 19;
- **removed node:** downstream consumers observe ordinary connection-set changes;
  old cache/snapshot references remain alive only as long as an old executable
  generation or published snapshot can still observe them; and
- **node/output without stable project identity:** use generation-local indexed
  cache state and discard it when that generation can no longer be observed.

Connection comparisons must use stable semantic endpoint identity rather than
builder handles, lowered node indices, or ORC-generation-local ordinals. A graph
edit entirely outside an indexed component therefore causes zero indexed
recomputation in that component.

Stable **cache identity** and cache **validity** are deliberately separate. The
same output can keep the same cache object across generations while only a subset
of pages becomes invalid for the candidate indexed semantic version. Published
old versions remain immutable and readable while replacement pages are computed;
unchanged pages may be structurally shared between published and candidate
snapshots.

This reconciliation phase happens after a candidate `CompiledGraph` exists but
before executable activation requires its indexed state to be ready. It may
schedule forward propagation and demand/tock work, but the JIT rebuild itself is
not an indexed mutation.

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
- the stable indexed-cache store/page payload arenas for identifiable `cache = true`
  outputs, plus per-generation endpoint bindings and generation-local cache state
  only for anonymous cached outputs;
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
- cached-page memory budgets, simple initial eviction policy, and later predictive/
  adaptive prefetch heuristics;
- exact fixed realtime lookahead/pre-roll defaults and any later adaptive or
  transport-prediction policy;
- the concrete executor mutation/change-notification queue ABI;
- cancellation versus discard policy for superseded candidate tocks;
- the exact stable indexed-output key encoding/schema-compatibility fingerprint
  used during executable-generation reconciliation;
- whether orphaned stable cache entries are released immediately when their node
  disappears or retained briefly under the general indexed-cache memory budget for undo-like
  churn; and
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
- callback families use the one-node `*_coverage` form today; any future
  `*_coverage_batch` form is a distinct multi-node lowering/execution API rather
  than an alias for one-node disjoint coverage;
- callback signatures are valid;
- missing reverse propagation has a correct conservative covered-input fallback;
- missing forward propagation has a correct conservative coverage/change
  fallback for indexed input or local-state changes;
- indexed sample/event declarations and the boolean `cache` contract are
  structurally valid;
- `max_events_per_index` is finite/nonnegative where applicable;
- realtime/indexed state lifecycle callbacks are usable by canonical
  `NodeLayout`/`NodeStorage`; and
- node-facing indexed reads can be validated against input coverage/live snapshot
  availability in appropriate builds.

Whole-project GraphJit validation additionally runs after all project connections
and explicit detach/feedback semantics are known. It must compute semantic SCC
membership over the complete logical dependency relation and reject any
`cache = true` indexed output with a consumer in the same semantic SCC as the
output's owning node. Diagnostics should identify the cached output, its owning
node, and at least one in-SCC consumer/path witness where practical.

Diagnostics should name the node type, offending callback/port, and expected
alternative whenever practical.

## 32. Implementation landing order

The indexed-port implementation should land in the following dependency order.
Each landing establishes the contract consumed by the next one; later phases
must not introduce compatibility paths back to the former compiled-port API.
Steps 1 and 2 are implemented; step 3 is the next capability landing.

1. **Indexed API and callback contract.** Introduce `IndexedRegion` and canonical
   owning `IndexedCoverage`; replace compiled-port declarations with distinct
   `IndexedInputConfig` and `IndexedOutputConfig { cache = true; }`; use
   `IndexedState` for the shared persistent indexed domain; and expose indexed sample/event access,
   `tock_coverage`, `propagate_forward_coverage`, and
   `propagate_reverse_coverage`. Remove the former extent/request-set and
   one-node `*_batch` interfaces rather than adapting them.
2. **Compiler-record and GraphJit wiring.** Carry the three one-node indexed
   callbacks and indexed-state layout through node traits, compiler records,
   package scanning/validation, resolved `NodeImplementation` metadata, and the
   lowering boundary. `CompiledGraph` retains its name because it denotes the
   JIT product rather than a port access model.
3. **Stable identity and static indexed planning.** Thread stable project
   instance/virtual-node/member/output identities into GraphJit, classify indexed
   connection directions, reject implicit realtime-to-indexed transport, compute
   whole-project semantic SCC membership, reject any `cache = true` indexed
   output whose consumer remains in the owning node's semantic SCC, and precompute
   indexed components, endpoint ordinals, forward/reverse/evaluation orders,
   convergence/conversion facts, cache contracts, and connection-set fingerprints.
   No mutable pages or semantic versions belong to this phase.
4. **Non-realtime `GraphExecutor` capability.** Add active/pending generations,
   canonical `NodeStorage`, the stable executor-owned cache store and
   per-generation bindings, transaction workspaces, semantic versions, creation
   coverage, connection reconciliation, forward invalidation, reverse demand,
   tock evaluation, stale-work rejection, and versioned external results. The
   transaction engine must support both dense sample pages and bounded packed,
   segmented event pages from its first complete form.
5. **Indexed-to-realtime live pull.** Publish one immutable indexed view per
   whole live block; bind prepared `cache = true` boundaries; lower inline
   realtime-compatible pulls through `cache = false` chains; reuse the existing
   direct/transient realtime sample and event planners; apply neutral/no-event
   behavior only outside coverage; and report missing cached data inside coverage
   as a readiness failure. Begin with fixed lookahead/pre-roll.
6. **Realtime recorder propagation and outward integration.** Let explicit
   recorder/source nodes mutate bounded authoritative indexed state and append
   bounded change notifications during `tick_block`; drain those notifications
   after the live block into ordinary versioned forward transactions; then expose
   coalesced versioned completion/change notifications to UI consumers. Once
   these indexed executor/query/visualization paths replace the legacy compiled-
   lane consumers, delete the compiled-lane execution, storage, RPC, and UI
   model outright; do not rename or adapt it into a second indexed system.
7. **Optimization only after capability is complete.** Add genuine multi-node
   `*_coverage_batch` operations and topology-permitted `tick_block_batch`
   grouping, improve page prediction/eviction and workspace liveness, and tune
   both the new indexed decisions and the existing GraphJit sample/event storage
   cost models. SIMD-aware layout/copy/conversion decisions, vectorization,
   fusion/direct forwarding, target-specific lowering, and generated hot-path
   assembly verification belong to this same final pass so cost weights are not
   tuned twice against an unfinished execution model.

Stable endpoint identity must exist before retained cache storage is allocated;
otherwise cache ownership becomes accidentally generation-local. Likewise,
indexed-to-realtime lowering follows the non-realtime executor because its
`cache = true` boundaries and published semantic snapshots are prerequisites,
while realtime-to-indexed flow remains an explicit recorder/source-node
capability rather than a fifth connection transport mode.

## 33. Summary invariants

1. `indexed` describes globally addressed, order-independent sample/event data;
   it does not mean JIT compilation or a storage class.
2. `IndexedCoverage` is the sole semantic domain boundary; there is no indexed
   extent/bounding-hull abstraction.
3. Coverage is a canonical union of sorted, nonempty, disjoint, non-adjacent
   half-open regions.
4. Node callbacks never request indexed values outside input coverage.
5. Indexed input coverage is the union of connected/mapped indexed output
   coverages and is visible to both tick and tock code.
6. `tock_coverage`, `propagate_forward_coverage`, and
   `propagate_reverse_coverage` each operate on one node; future
   `*_coverage_batch` APIs, if added, mean multiple nodes processed together.
7. `cache` is a boolean scheduling/materialization contract and defaults to
   `true`; it is not a generic retention preference.
8. `cache = false` outputs own no indexed cache pages. Their tock/reverse work is
   realtime-compatible by contract when used on a live pull path, and they write
   into caller, transaction, transient, or direct consumer storage.
9. `cache = true` outputs are retained materialization boundaries. Realtime never
   invokes their tock as a fallback; required pages/direct data must be prepared
   before consumption.
10. Cache pages are canonically aligned, but page boundaries never widen coverage.
11. One cached page is either wholly valid or wholly invalid for its exact
    `page_interval & coverage` domain and indexed semantic version.
12. Sparse UI demand at a cached output selects pages; touching an invalid page
    selects its entire covered page domain. Sparse demand at an uncached output
    remains exact because there is no page to promote it to.
13. Forward change propagation remains exact and is never widened to page
    boundaries merely because a cached page becomes invalid.
14. Reverse demand remains exact through uncached outputs. At a cached output,
    valid pages terminate traversal and missing/invalid pages promote demand to
    their complete covered page domains before reverse propagation continues.
15. `tock_coverage()` receives only coverage that actually needs computation:
    selected covered page domains for cached outputs and exact demand for
    uncached outputs.
16. Arbitrary node-local mutations may trigger forward processing with zero
    changed indexed inputs and may change output coverage and/or values.
17. Added or removed coverage is itself a semantic change propagated downstream.
18. Forward propagation never forces tock evaluation and continues through
    uncached intermediates.
19. Forward and reverse propagation are opposite graph-direction dependency
    queries, not mathematical inverses.
20. Sample cached payloads may be dense or coverage-packed; this choice is
    independent of whole-page validity.
21. Event cache pages use whole-page validity plus packed ordered events, with
    semantic capacity based on `max_events_per_index * covered_positions`.
22. Indexed event reads may span cached pages and therefore use segmented ordered
    iteration rather than requiring contiguous storage.
23. Fixed node/indexed persistent state remains in canonical `NodeStorage`;
    dynamically growing cached indexed pages live in an executor-owned stable
    cache store with per-generation endpoint bindings. Uncached outputs have no
    such page store.
24. Non-realtime indexed transactions may use reusable dynamic workspace/arena
    storage; request-sized temporaries need not live in `NodeStorage`.
25. Coverage publication is a node-creation rule, not a project-startup rule:
    every newly introduced semantic concrete node establishes indexed output
    coverage before demand can target it, while retained stable nodes reuse prior
    coverage unless an actual forward cause changes it.
26. All mutations affecting indexed semantics enter through an executor-owned
    boundary; realtime sources report bounded changes for later executor-side
    propagation.
27. Indexed validity/results are semantic-versioned. Work computed for an obsolete
    semantic version cannot commit as valid for a newer version.
28. Realtime indexed pull paths may execute only `cache = false`
    `tock_coverage()` work inline. GraphJit should use direct placement whenever a
    single compatible no-history consumer permits it and otherwise use bounded
    compiler-owned transient live storage.
29. A live read outside indexed coverage produces the realtime input's
    `neutral_value`/no events without materializing indexed storage. A missing
    `cache = true` page **inside** coverage is a readiness failure, not neutral
    data and not permission for an audio-thread tock.
30. `cache = true` live boundaries use ahead-of-time page readiness, initially via
    a simple fixed lookahead/pre-roll policy. Cached pages may be evicted once
    unpinned; advanced prediction/adaptive eviction is optional policy work.
31. Realtime execution observes one immutable published indexed semantic version
    for an entire live graph block. Semantic-version publication and page-view
    publication happen only at whole-live-graph boundaries.
32. A candidate semantic version need prepare only cached boundaries required by
    imminent live use; uncached live outputs need no page precomputation. The old
    published semantic version may remain active while those cached boundaries are
    prepared.
33. JIT compilation alone is not indexed invalidation. Stable concrete-node/output
    identities rebind executable generations to existing cache storage without
    copying payloads; only actual semantic differences affect candidate validity.
34. Connection-set addition/removal/replacement for one indexed input
    conservatively marks the whole logical input changed over
    `old_input_coverage | new_input_coverage`.
35. External incomplete candidate data is pending/not-ready, never fabricated as
    empty/default. Completed external results identify their semantic version.
36. Cached indexed outputs cannot participate in their own semantic feedback.
    After whole-project semantic SCC detection, every connection sourced from a
    `cache = true` indexed output must target a node outside the owning node's
    semantic SCC. Nodes with cached indexed outputs may themselves be in SCCs;
    those cached outputs must strictly leave the SCC. `cache = false` indexed
    outputs may remain inside an otherwise-valid explicit realtime SCC because
    they own no pages and promise realtime-compatible inline materialization.

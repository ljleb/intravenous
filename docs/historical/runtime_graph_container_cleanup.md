# Runtime graph container and `constexpr` legacy audit

## Purpose

The graph builder/lowerer/compiler used to execute during constant evaluation.
That requirement no longer exists: configured graphs are reconstructed and
lowered at runtime, and the former `consteval` compiler tests are ordinary
runtime tests. This note records container choices and helper shapes that still
reflect the old compile-time constraint, so a later cleanup can evaluate them
without rediscovering the history.

This is an audit, not a migration plan. Container changes should preserve the
existing ordering/deduplication invariants and should be measured against real
package reload/configuration workloads before being kept.

## Historical reason these structures exist

`docs/historical/phase4_constexpr_migration_scope.md` explicitly required the existing
builder, lowering, and compiler to execute during constant evaluation. The
migration therefore made the live call chain `constexpr` and kept all transient
storage usable in constant evaluation.

The history makes the motivation particularly explicit:

- `c9534ef4` — `Make compiler helper chain constexpr`;
- `ae42384c` — `Make detach path search constexpr`;
- `b6cff495` — `more constexpr unordered collections`;
- `10946ddd` — `phase4: complete constexpr lowering header migration`.

`b6cff495` introduced `details::ConstexprHashMap` and
`details::ConstexprHashSet`. Their source comment says they exist as small
constexpr open-addressing containers and specifically contrasts them with the
repeated element shifting of `flat_map`/`flat_set` during constant evaluation.

That constraint was later removed. `docs/historical/llvm_module_reload_migration.md`
records that the former `consteval` compiler checks became ordinary runtime
tests and that each lowering pass now owns a runtime `ConfiguredGraph` copy.
The graph implementation nevertheless still contains the constexpr-specific
containers, pervasive `constexpr` qualifiers, and several `flat_map` /
`flat_set` representation choices made while constant evaluation was a primary
performance concern.

## Main conclusion

There is no longer a reason to preserve a container merely because it is
constexpr-friendly. There are also enough dense numeric domains in the lowerer
and compiler that replacing every custom container with `std::unordered_map`
or `std::unordered_set` would miss the larger opportunity. In several places
the best runtime representation is likely a vector/offset table or bitmap,
not a general associative container.

The highest-confidence cleanup is therefore:

1. remove associative containers from dense numeric domains;
2. keep closed edge collections as vectors with explicit sorted/unique
   invariants instead of `flat_set` where lookup is not required;
3. replace or retire `ConstexprHashMap` / `ConstexprHashSet` only after their
   remaining sparse-key users have been classified;
4. remove dead retained maps/sets rather than selecting a faster container for
   them;
5. treat package-finalizer data structures separately: that tool was not built
   around the old graph-consteval constraint.

## High-confidence candidates

### 1. `ExecutableGraphData::{edges,event_edges}`

Current representation:

```cpp
std::flat_set<GraphEdge> edges;
std::flat_set<GraphEventEdge> event_edges;
```

These collections are closed executable-IR data. Production compiler code
iterates them to construct connectivity and scheduling analyses. There is no
production `find()`/`contains()` lookup against either set after the IR is
closed; the only direct `contains()` use found is in a compiler test.

The lowerer already has explicit canonicalization points that sort and
uniquify edge batches. `apply_node_permutation()` likewise extracts the edges,
remaps them into vectors, sorts/deduplicates them, and constructs a new
`flat_set` only to restore the current field type.

A `std::vector<GraphEdge>` / `std::vector<GraphEventEdge>` with the invariant
"sorted and unique at the `ExecutableGraphIR` boundary" is therefore a strong
candidate. Lookup-oriented compiler work should use the one compiler-local
connectivity index instead of requiring an associative IR representation.

Benefits:

- removes `flat_set` insertion/erase shifting from completed-IR manipulation;
- avoids converting vector -> `flat_set` after canonicalization;
- makes the semantic invariant explicit rather than encoding it in a container;
- removes one reason graph code requires `<flat_set>` from the standard
  library implementation.

Caveat: event fan-in/fan-out completion currently rewires `g.event_edges` with
`erase()`/`insert()` before the final canonicalization point. A migration should
batch those rewrites or accept temporary unsorted vector state rather than
mechanically substituting vector operations one-for-one.

### 2. `GraphBuildArtifact::{edges,event_edges}`

`GraphBuildArtifact` currently stores another pair of `std::flat_set`s. The
`Graph` constructor immediately copies them into its runtime
`std::vector<GraphEdge>` and `std::vector<GraphEventEdge>` fields.

This is a particularly weak reason to retain a flat associative container: the
artifact is a construction DTO and does not perform associative lookup. If the
compiler already owns canonical edge vectors, the artifact can move those
vectors directly into `Graph` and avoid one representation conversion/copy.

### 3. Compiler connectivity maps over concrete ports

`CompilerConnectivity` currently contains:

```cpp
ConstexprHashMap<ConcretePortId, std::vector<GraphEdge>, ...> sample_targets;
CompilerSampleSources sample_source;
ConstexprHashMap<ConcretePortId, std::vector<GraphEventEdge>, ...> event_targets;
ConstexprHashMap<ConcretePortId, GraphEventEdge, ...> event_source;
```

`CompilerSampleSources` is already the better model for a large part of this
problem: node input ports are a dense domain, so it computes per-node offsets
and stores source entries in flat vectors.

The other compiler connectivity relations have similarly enumerable domains:
node sample outputs, node event outputs, node event inputs, plus small graph
boundary domains. They can likely use the same offset-table strategy instead
of hashing `{node, port}` on every query.

This would remove several hot `ConstexprHashMap` uses at once and keep the
compiler connectivity analysis contiguous in memory. It is a stronger runtime
optimization candidate than replacing those maps with node-based
`std::unordered_map`.

### 4. Dense `size_t -> size_t` maps

Several custom hash maps use IDs that are already indices into vectors.
Examples include:

- `compile_dormancy_groups()`'s `remapped_group_indices`;
- `GraphLowerer::build_topology_scope_memberships()`'s `subgraph_order`;
- `GraphLowerer::build_lowered_scopes()`'s `scope_index`;
- `GraphLowerer::subgraph_by_boundary`, because `NodeBundleHandle` is
  `size_t` and bundle handles are allocated from the node-bundle collection.

These should not need hashing. A vector initialized to a sentinel such as
`GRAPH_ID`, or `std::vector<std::optional<size_t>>` where absence must be
explicit, gives O(1) lookup with substantially less machinery and allocation.
The correct vector size is already known at each call site.

`remapped_group_indices` is the clearest example: its key is an old dormancy
group index in `[0, groups.size())`.

### 5. Dormancy membership and frontier sets

`compile_dormancy_groups()` builds one `ConstexprHashSet<size_t>` for every
lowered subgraph solely to answer whether a node belongs to that group.
`compile_lowered_subgraphs()` sorts `member_nodes` before these groups reach the
dormancy compiler, so membership can use `std::binary_search()` directly, or a
single reusable dense generation/stamp array when graph size makes that
worthwhile. Retaining a separate open-addressing set per group duplicates the
membership data and allocations.

The same function creates `seen_sample_inputs`, `seen_event_inputs`, and
`seen_sample_outputs`. The first two appear redundant because each concrete
input port is visited exactly once by the surrounding `(node, input)` loops.
The sample-output frontier set also appears redundant once the closed-IR
invariant of at most one sample source per concrete input/public output is
relied upon. That last removal should be guarded by a focused invariant test,
but these sets are good candidates for deletion rather than replacement.

### 6. `region_internal_latency()` input-latency map

`region_internal_latency()` uses:

```cpp
ConstexprHashMap<ConcretePortId, size_t, ConcretePortIdHash> input_latencies;
```

The compiler already defines `InputPortLatencyTable`, an offset-based dense
table for the same kind of concrete input-port domain later in the file. The
region-local analysis can use a similar dense table (or a reusable scratch
vector scoped to the region) instead of hashing every input edge.

This is another case where the old constexpr-capable hash abstraction obscures
a naturally dense runtime domain.

### 7. `ExecutableGraphData::detached_reader_outputs`

`detached_reader_outputs` is populated by lowering, copied into
`ExecutableGraphData`, remapped by `apply_node_permutation()`, and is not read
by production compiler code afterward. Its only direct query found outside the
lowerer/compiler implementation is a test assertion.

This appears to be dead retained IR state. It should be verified and removed
rather than migrated to a different container.

### 8. `detached_info_by_source` after lowering

The lowerer's temporary lookup by detached source is meaningful while detach
semantics are being assembled. In the closed executable IR, however,
`DetachedInfo::original_source` duplicates the map key, and the compiler later
only iterates the map to produce `std::vector<DetachedInfo>`.

The post-lowering representation can therefore likely be a vector of
`DetachedInfo`, while any source-keyed lookup required during lowering remains
lowerer-local scratch. This would also simplify node permutation: remap the
fields of each `DetachedInfo` directly instead of rebuilding a sorted map.

### 9. `virtual_node_ids_by_backing_node_id`

This string-keyed `flat_map` is built during lowering, transferred through
`ExecutableGraphIR`, and then moved into `GraphBuildMetadata`. No production or
test consumer of `GraphBuildMetadata::virtual_node_ids_by_backing_node_id` was
found in the current tree.

If that remains true after checking downstream/private consumers, the best
optimization is removal of the field and the map construction entirely. If it
is retained for a future API, its build-time lookup can use an ordinary runtime
hash map and canonicalize only if deterministic serialized order is actually
required.

### 10. Lowering workspace topology edge sets

`LoweringWorkspace` still maintains `std::flat_set<TopologyEdge>` and
`std::flat_set<TopologyEventEdge>` plus separate pending vectors. The pending
vectors already acknowledge that inserting every edge into a sorted flat set is
expensive: edges are batched and normalized only before operations that require
canonical state.

A runtime-oriented representation can likely finish this transition: keep edge
vectors plus a dirty/canonical state local to the lowerer, then sort/unique at
the few operations that require ordered lookup or deterministic traversal.
`replace_sample_ports()` / `replace_event_ports()` already extract the whole
flat-set backing sequence, mutate it, and replace it, which is effectively a
vector batch transform hidden behind the associative type.

This change is somewhat more invasive than changing the closed IR because the
lowerer performs rewrites while topology is being built, so it should follow
the simpler IR/artifact migration.

## Medium-confidence candidates

### 11. Remaining `ConstexprHashMap` / `ConstexprHashSet` users in lowering

The custom containers remain throughout `GraphLowerer`, including:

- boundary-source mappings;
- configured connectivity sets;
- runtime-binding mappings;
- materialized event-output mappings;
- source/target maps used while completing topology;
- temporary event fan-in/fan-out groupings;
- assigned-output/seen-port sets.

The class implementation uses open addressing over
`std::vector<std::optional<T>>`, grows at a 0.5 load factor, has no caller-facing
`reserve()`, and has a custom bucket-order iterator. Those choices were
reasonable under the old constant-evaluation constraint, but they should not be
assumed optimal for runtime.

Do **not** mechanically replace all of these with `std::unordered_*`.
Classify them first:

- dense handle/index domains -> offset vectors/bitmaps;
- write-once/read-many sparse relations -> runtime hash table or sorted vector;
- tiny collections -> vector plus linear search may be cheaper;
- deterministic iteration requirements -> explicit sorting at the boundary,
  not accidental dependence on a container's iteration order.

For sparse runtime hashing, `std::unordered_*` is the simplest standard option,
but its node-based allocation can be less cache-friendly than the current open
addressing table. If these maps show up materially in profiling, a runtime
flat/dense hash implementation is a better comparison than assuming
`std::unordered_*` wins.

### 12. Configured-connectivity membership sets

`LoweringConfiguredConnectivity` builds four hash sets of sample/event input
and output IDs and then performs many membership queries while projecting
metadata.

The IDs are composed of node-bundle handles, port indices, and for samples,
channel indices. All three dimensions are known from the node bundles. Dense
bitmaps or offset-addressed byte vectors are therefore possible and likely more
cache-friendly than hashing. This is worth considering if introspection
projection remains visible in graph configuration profiles.

### 13. `GraphBuilderConnections::sample_lowering_plan()`

Grouping configured sample connections currently uses
`ConstexprHashMap<NodeBundlePortId, size_t, ...>`. This is another legacy
constexpr container outside the lowerer proper. Since bundle handles and port
indices are bounded by the configured node bundles, a dense index can be
supplied by the builder/lowerer, or the relatively small connection list can be
sorted/grouped once.

The right choice depends on actual connection counts, so this is less urgent
than the compiler's dense tables.

### 14. `std::vector<bool>` scratch bitmaps

The graph path uses `std::vector<bool>` for several runtime scratch bitsets,
including Tarjan `on_stack`, lowerer visited/claimed/projected state, and bound
input entries. This is not specifically a constexpr workaround, but it is worth
revisiting now that runtime performance is the only concern. The packed proxy
representation saves memory but can be slower than `std::vector<uint8_t>` for
small/medium graph worksets.

This should be benchmarked rather than changed categorically. It is lower
priority than removing hashes from dense domains.

### 15. Sorted ready vectors in SCC scheduling

Within each SCC, `build_execution_plan()` maintains a sorted `ready` vector,
removes `ready.front()` with `erase(begin())`, and inserts newly-ready nodes via
`lower_bound()` plus vector insertion. The ordering is intentionally
deterministic, but the representation makes every pop and many inserts O(n)
shifts.

At runtime, a min-heap (`std::priority_queue` with `std::greater`) can preserve
the same smallest-node-first ordering with O(log n) push/pop. For typical tiny
SCCs the vector may still win, so this is a benchmark candidate rather than an
immediate correctness cleanup. The region-DAG topological order already uses
an append-only ready vector and does not have this issue.

## Containers that are not obviously wrong

### `std::vector`

The Phase 4 design deliberately kept transient vectors during constant
evaluation, but vectors are also the natural runtime representation for most
ordered graph data. Their historical constexpr use is not itself a reason to
replace them.

### Custom open-addressing hash tables, considered purely at runtime

Although `ConstexprHashMap` / `ConstexprHashSet` are historical abstractions,
the underlying open-addressing layout can still outperform
`std::unordered_map`/`std::unordered_set` for POD-like keys because it avoids
per-entry node allocation. Their problem is primarily that the implementation
and API are frozen around a requirement that no longer exists. Replace them
because a better domain-specific/runtime representation exists, not merely
because their names contain `Constexpr`.

### Sorted deterministic edge order

Several compiler algorithms and tests rely on deterministic order. Removing
`flat_set` must not accidentally make order depend on a hash table. Prefer
vector + explicit `sort`/`unique` at the semantic boundary when order matters.

## Package finalizer audit

`tools/iv_package_finalize.cpp` is not part of the old graph-consteval
pipeline. It already uses ordinary runtime containers and LLVM's runtime ADTs:
`std::vector`, `std::unordered_set`, `std::set`, `llvm::SmallVector`, and
`llvm::SmallPtrSet`.

There are possible micro-cleanups there—for example the duplicate-definition
validation uses `std::set` where an unordered set could perform cheaper lookup
if ordering is irrelevant—but those choices are not constexpr legacy and
should be profiled independently. This audit does not recommend changing the
package finalizer merely to make its containers resemble the graph compiler.

The historical **graph builder finalizer** is a different matter. It was made
`consteval` during Phase 4 and later deleted by `7df64c37` (`flatten everything
for real now`); its responsibilities now live in the lowerer/compiler path.
That is why the remaining legacy is concentrated in
`graph/builder/lowering.hpp`, `graph/compiler.h`, and the graph IR/build DTOs
rather than a current `GraphBuilderFinalizer` type.

## Pervasive `constexpr` surface

Most GraphLowerer helpers and many GraphCompiler helpers are still declared
`constexpr`. At runtime this is generally not a direct performance problem—the
optimizer can compile them as ordinary functions—but it unnecessarily
constrains which library facilities and implementation techniques appear
acceptable, and it obscures the fact that compile-time graph execution is no
longer supported.

A cleanup can remove `constexpr` from runtime-only helper chains after the
container migration. This should be treated primarily as maintenance and API
clarification, not as an expected runtime speedup.

Related stale names/text also remain:

- `tests/graph_compiler_consteval_gtest.cpp` is now a runtime test despite its
  filename;
- the file contains repeated `// runtime check` remnants;
- `docs/builder_lowering_pipeline_design.md` still describes
  `GraphBuilder::finish()` / `build()` as `consteval`, calls its optimization
  section "Compile-time optimization targets", refers to constexpr graph
  tests/profiling, and says intermediate artifacts should remain
  constexpr-friendly.

`docs/historical/phase4_constexpr_migration_scope.md` is already clearly marked historical
and can remain as archaeology. The builder/lowering design document should
instead be updated when this cleanup is undertaken so it does not continue to
impose a superseded implementation constraint.

## Suggested cleanup order

A low-risk sequence would be:

1. add focused runtime timing around `GraphLowerer::lower()` and
   `GraphCompiler::compile()` on representative graphs (`saw`, `q24_pan`,
   `q24_icosphere_pan`);
2. remove demonstrably dead retained state (`detached_reader_outputs`, and
   `virtual_node_ids_by_backing_node_id` if no consumer exists);
3. make `ExecutableGraphData` and `GraphBuildArtifact` edge storage vector-based
   with explicit sorted/unique invariants;
4. replace dense numeric hash maps with offset vectors/sentinel tables;
5. make `CompilerConnectivity` fully dense by concrete port domain;
6. revisit remaining sparse `ConstexprHash*` uses using measured runtime data;
7. only then remove the now-unused constexpr-hash implementation and stale
   `<flat_map>` / `<flat_set>` dependencies;
8. remove misleading runtime-only `constexpr` qualifiers and rename stale
   consteval tests/docs.

This order avoids a large mechanical container rewrite and gives each step a
measurable purpose.

## Expected effects

The most likely improvements are graph configuration/lowering/compiler latency,
allocation count, and standard-library portability. None of these changes
should affect audio-thread DSP execution after a graph has been built; they are
build/reload-path improvements.

Removing all graph uses of `<flat_map>` / `<flat_set>` would also eliminate the
specific standard-library-header dependency that currently prevents building
this graph path with otherwise capable older libstdc++ installations. It would
not by itself lower the project's C++ language requirement or guarantee that an
older toolchain can build the full repository.

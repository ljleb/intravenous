# Modular builder-lowering pipeline

## Purpose

Make builder lowering a sequence of ordinary, independently callable
transformations. This makes correctness tests and compile-time profiling target
the same production units, rather than relying on diagnostic-only cut points.

## Architectural direction

The authored graph has the same compiler role as an AST/HIR: it is the stable,
loss-minimizing representation of the structure the author intended. It is
not literally syntax, but it must have the same ownership boundary.

`GraphBuilder` is an authoring convenience. Its only compilation-adjacent
responsibility should be to finish an immutable `AuthoredGraph` value:

```cpp
consteval AuthoredGraph GraphBuilder::finish() &&;
```

`AuthoredGraph` owns node bundles, authored connections, public ports, detach
declarations, source annotations, and virtual-node declarations. Once it is
finished, the builder no longer participates in lowering, metadata production,
or runtime graph construction.

The compiler has two semantic representations and one mechanical construction
DTO:

```text
AuthoredGraph (HIR)
  -> ExecutableGraphIR
  -> GraphBuildArtifact (construction DTO)
  -> Graph (frozen runtime value)
```

`LoweredTopology`, `PreparedGraph`, execution plans, SCCs, buffer plans, and
dormancy groups are not additional semantic graph representations. They are
internal lowering workspaces or analyses of `ExecutableGraphIR`.

Each semantic transition has one owner and one invariant:

- `GraphLowerer::lower(AuthoredGraph const&)` resolves authored structure,
  tiling, channel semantics, defaults, routing, hierarchy, detach behavior,
  and provenance into a closed `ExecutableGraphIR`.
- `GraphCompiler::compile(ExecutableGraphIR const&)` validates, schedules,
  lays out, and freezes that fixed semantic graph through a temporary
  `GraphBuildArtifact` into `Graph`.

No semantic node or edge may be created after `ExecutableGraphIR` is finished.
The compiler may reorder, wrap, schedule, allocate buffers for, or freeze its
nodes, but discovering a required `Broadcast`, `ConnectionNode`, default, or
sink after this boundary is a lowering bug.

`GraphBuilder` should eventually expose one ordinary result-producing method:

```cpp
consteval CompiledGraph GraphBuilder::build() &&;

struct CompiledGraph {
  Graph graph;
  GraphBuildMetadata metadata;
  GraphIntrospectionMetadata introspection;
};
```

Metadata is a product of compilation or an explicit inspection pass over a
named representation; it is not an alternate semantic build path. The current
`build_metadata`, `build_root_node`, `build_execution_root_node`, and
`build_execution_root_node_with_metadata` APIs are transitional compatibility
facades to be removed after their consumers migrate.

## Current state

`GraphBuilder` owns authored node bundles, connections, public ports, virtual
nodes, and detach declarations. `GraphBuilderLowering::lower()` creates one
private mutable `Lowerer`, whose `run()` method projects bundles, lowers sample
and event connections, handles detach declarations, and normalizes the result.

The stored authored data is modular, but the substantial work is not: sample
grouping, direct-route decisions, channel resolution, generated connection-node
construction, vacant-input lowering, and subgraph binding all mutate one
`LoweredBuilderGraph` through private helpers. A final graph test therefore
cannot isolate which unit owns a cost or a regression.

## Target pipeline

```text
AuthoredGraph
  -> BundleProjection
  -> SampleLoweringPlan
  -> ConnectedSampleLowering
  -> VacantSampleInputLowering
  -> SubgraphSampleBindingLowering
  -> event/detach lowering and normalization
  -> ExecutableGraphIR
  -> GraphBuildArtifact
  -> Graph
```

The named lower-level operations are internal passes of `GraphLowerer`; they
need not become public graph representations. They have explicit contracts and
ordinary test/profiling entry points, but share one lowering context and finish
by producing the single self-sufficient executable IR.

## First extraction: sample lowering

The first refactoring boundary is between semantic planning and topology
materialization.

`SampleLoweringPlan` will own, per target logical sample port:

- the authored connection group;
- validated source and target channel counts;
- resolved source and target channels;
- exact logical-port identity and direct-routing eligibility;
- conversion plans;
- runtime-binding lookup results where applicable.

The plan is computed from authored graph state plus the lowering context's
bundle projection.
`ConnectedSampleLowering` consumes it to append generated nodes and edges. It
does not repeat source/target resolution or discover the same logical port by a
global search.

Vacant-input and subgraph-binding passes remain separate consumers of the
materialized result. This retains the current lowering order and permits
incremental extraction without changing the final static execution
representation.

## Contracts and tests

Every extracted unit needs a focused constexpr test in addition to final graph
tests.

| Unit | Contract examples |
|---|---|
| Bundle projection | concrete, tiled, and boundary ports map to expected topology endpoints |
| Sample plan | native ordered channels are direct; reordered channels are not; channel identities and types validate |
| Connected materialization | direct plans create edges; conversion/fan-in plans create the expected connection nodes |
| Vacant-input lowering | each unbound concrete input receives one default path |
| Metadata indexing | disconnected, connected, and mixed ports retain their reported connectivity |

Tests should call the production unit directly where its inputs can be built
without unrelated lowering. End-to-end builder tests remain the compatibility
check.

## Profiling model

Compile-time profiling is performed by small benchmark translation units that
call the normal unit API:

```cpp
constexpr auto plan = plan_sample_lowering(authored.connections);
```

Measurements are cumulative only when the input artifact necessarily depends
on an earlier unit. Individual phase costs are obtained by compiling the
smallest valid unit invocation, not by adding destructive early returns inside
the lowering implementation.

## Migration rules

- First introduce `AuthoredGraph` as a value type and route all existing build
  methods through `finish()`. Do not add further public compiler entry points
  to `GraphBuilder`.
- Preserve existing `GraphBuilder` build methods only as temporary compatibility
  façades over the explicit compiler pipeline.
- Move all semantic synthesis currently spread across `Lowerer` and
  `GraphBuilderFinalizer` into `GraphLowerer`; in particular subgraph defaults,
  fan-in/fan-out, dangling-port completion, and scope construction must occur
  before executable IR is finished.
- Carry source, virtual-node, and authored-node provenance forward into
  executable nodes. Later compilation must not need `GraphBuilderNodeBundles`,
  `GraphBuilderConnections`, or stringified node IDs to recover authored
  meaning.
- Treat scopes as executable-IR hierarchy metadata, not pseudo executable
  `StoredNode`s that later passes skip and traverse around.
- Do not introduce a second graph representation solely for profiling.
- Keep intermediate artifacts constexpr-friendly and move-only only where
  ownership demands it.
- Move a repeated computation into the plan only when its result is consumed by
  more than one materialization decision or it removes graph-wide rediscovery.
- Reprofile after each extraction; do not infer performance from code shape.

## Completion criterion

The sample-lowering units can be built and tested individually, the existing
end-to-end graph tests preserve behavior, and the benchmark no longer depends
on diagnostic compile stages or destructive lowering modes.

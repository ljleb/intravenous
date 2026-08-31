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

The compiler has exactly three representations and two conversions:

```text
AuthoredGraph (HIR)
  -> ExecutableGraphIR
  -> Graph (frozen runtime value)
```

`LoweredTopology`, `PreparedGraph`, execution plans, SCCs, buffer plans, and
dormancy groups are private scratch or analyses within one of those two
conversions. They are not additional representations and have no public
entry points.

Each semantic transition has one owner and one invariant:

- `GraphLowerer::lower(AuthoredGraph const&, GraphLoweringOptions)` resolves authored structure,
  tiling, channel semantics, defaults, routing, hierarchy, detach behavior,
  and provenance into a closed `ExecutableGraphIR`.
- `GraphCompiler::compile(ExecutableGraphIR)` validates, schedules,
  lays out, and freezes that fixed semantic graph into `Graph`.

No semantic node or edge may be created after `ExecutableGraphIR` is finished.
The compiler may reorder, wrap, schedule, allocate buffers for, or freeze its
nodes, but discovering a required `Broadcast`, `ConnectionNode`, default, or
sink after this boundary is a lowering bug.

`GraphBuilder` exposes one ordinary result-producing convenience method:

```cpp
consteval CompiledGraph GraphBuilder::build(GraphLoweringOptions = {}) &&;

struct CompiledGraph {
  Graph graph;
  GraphBuildMetadata metadata;
  GraphIntrospectionMetadata introspection;
};
```

Metadata is a product of the `AuthoredGraph -> ExecutableGraphIR` conversion;
it is not an alternate semantic build path. The former
`build_metadata`, `build_root_node`, `build_execution_root_node`, and
`build_execution_root_node_with_metadata` façades have been removed. Callers
either use the explicit two conversions or the single `GraphBuilder::build`
convenience.

## Current state

`GraphBuilder` owns authored node bundles, connections, public ports, virtual
nodes, and detach declarations. Finishing transfers those into `AuthoredGraph`.
`GraphLowerer::lower()` is the sole public lowering entry point. One
`GraphLowerer` instance carries the authored references and scratch state for
the whole authored-to-IR conversion.

## Target two-conversion architecture

```text
AuthoredGraph
  -> GraphLowerer::lower() // authored graph -> closed executable IR
  -> GraphCompiler::compile(std::move(ir)) // executable IR -> fixed Graph
  -> Graph
```

`GraphLowerer` is one stateful class for the whole authored-graph-to-IR
conversion. It owns the authored reference, lowering options, the topology
under construction, and plain nested scratch structs. Its public macro step is
one function, `lower()`, which composes smaller member functions.

`GraphCompiler` is the symmetric other half of compilation: it consumes the
closed executable IR and produces the frozen runtime `Graph` (and the static
data structures carried by `CompiledGraph`). It likewise has exactly one public
macro step, `compile(ExecutableGraphIR)`. Scheduling, validation, subgraph
compilation, dormancy analysis, artifact construction, and metadata extraction
are private implementation steps of that one IR-to-runtime conversion; none is
another representation boundary or a public pass API.

The current surface already has that single public `GraphCompiler::compile`
entry point, but its implementation helpers are `details` free functions in
`graph/compiler.h`, rather than members of a stateful compiler object. That is
fine: the important constraint is the single conversion boundary, not a
symmetrical object-oriented implementation. Helpers may remain ordinary private
functions and plain data; do not introduce a deeply nested compiler-object
hierarchy merely to match `GraphLowerer`.

An operation that takes and returns `ExecutableGraphIR` is not a third
conversion phase. It is an internal helper of the conversion that owns it.
For now, executable-IR completion belongs to `GraphLowerer`, immediately
before it returns the closed IR. Such helpers may only consult the IR they
receive; `GraphCompiler` therefore never needs a completion-state flag.

`GraphCompiler::compile` is the sole conversion from executable IR to the fixed
runtime `Graph`. No intermediate graph, diagnostic build mode, or profiling API
is exposed.

## Deferred event fan-out

Sample fan-out no longer lowers a `Broadcast` node: layout-compatible consumers
share one output ring, each `InputPort` owns its read cursor, history, and
validated latency, and only conversion branches receive a wrapper-managed copy.

Event fan-out intentionally remains materialized through `BroadcastEvent` for
now. `EventSharedPortData` currently owns its read index, so aliasing an event
buffer would let one consumer advance or clear events needed by another. The
future event equivalent must first make the reader state consumer-owned in
`EventInputPort` and give shared event storage a retention rule sufficient for
all readers. Only then may event lowering stop materializing `BroadcastEvent`:
same-type consumers can alias one event store, while type-converting consumers
can receive compiler-managed conversion branches. Do not remove the event
broadcast node before that reader/retention split exists.

## Conversion ownership and information flow

The implementation should make the full path from `GraphBuilder` to the static
runtime result straightforward to follow. A piece of work belongs to the
conversion that owns the semantic transition it performs:

This is also a physical source-organization rule, not merely an API rule. The
required layout has exactly one self-contained implementation file for each
owner:

- one `GraphBuilder` file for the authoring convenience and its authored-data
  construction;
- one `GraphLowerer` file for `AuthoredGraph -> ExecutableGraphIR`; and
- one `GraphCompiler` file for `ExecutableGraphIR ->` the static data and
  frozen runtime `Graph` that actually execute.

The authored and executable IR data types may be shared definitions, but they
must not become a collection of pseudo-pass headers. Existing fragmented
`graph/builder` headers are a migration artifact to be consolidated before
further compiler optimization work. The single-file rule is valuable now
because it makes repeated recovery, misplaced work, and accidental extra
representations immediately visible.

`dsl.h` is explicitly outside this rule. It provides syntactic extensions for
clients of `GraphBuilder`; it is not builder storage, lowering, or compilation.

- `GraphLowerer` creates and closes executable meaning: generated nodes and
  edges, defaults, routing, hierarchy bindings, detach semantics, runtime-port
  adapters, provenance, and introspection.
- `GraphCompiler` consumes that fixed meaning only to validate, order, schedule,
  allocate, lay out, and freeze the runtime graph and its static data.

If `GraphCompiler` discovers or synthesizes executable graph meaning, move that
work to `GraphLowerer`. If `GraphLowerer` performs a scheduling, allocation, or
runtime-layout decision, move it to `GraphCompiler`. The rule is about the
meaning of the work, not which helper happens to contain it today.

Likewise, preserve information at the point where it is known when a later
step needs it. Do not discard a resolved routing decision, ownership relation,
binding lookup, or edge classification only to reconstruct it later by scanning
authored data or the lowered topology. Carry a narrow phase fact or index
forward instead. A later scan is appropriate only when it is genuinely a new
analysis of the completed representation, rather than recovery of an earlier
fact.

## Compile-time optimization targets

The following are known information-loss or repeated-analysis targets in the
two conversions. They are ordered by expected compile-time effect.

1. `GraphCompiler` must build its private connectivity analysis once. Region
   scheduling, per-region latency, dormancy frontiers, sample fan-out, event
   bindings, and graph latency must consume that one analysis rather than each
   rebuilding source/target maps or rescanning every edge. In particular, do
   not scan all edges once per region or once per dormancy scope. This index is
   compiler-local scratch, not another IR representation.
2. A generated topology node must retain its scope membership. Do not append it
   without provenance and later infer membership separately for every subgraph
   by fixed-point traversal of topology edges. Scope membership is executable
   IR facts and should be carried directly into the lowered scope records.
3. Structural node and port references in executable IR must use stable numeric
   handles, not node-id strings. If compilation reorders nodes, apply the
   permutation to those handles. Strings are stable runtime/export identities,
   not compiler-internal addresses.
4. A completed sample edge includes its channel-conversion plan and must leave
   lowering with that fact populated. Compiler buffer planning must distinguish
   per-consumer read requirements from shared-storage requirements, rather than
   calculating target plans and merging them into a selected fan-out owner.
   Similarly, dormancy keeps concrete port references until its final static
   export IDs and latencies are both available.
5. Lowering metadata must retain results it has already produced, including the
   virtual-node IDs by backing node. Connectivity queries used for virtual and
   public-port metadata need one lowering-local authored-connectivity index,
   not a scan of every authored connection per port.

Small cleanup targets follow the same rule: retain the next construction-order
counter instead of repeatedly finding a maximum, avoid copying immutable
detach-validation adjacency, and use the compiler connectivity index for event
output bindings.

## Inside authored lowering

`GraphLowerer::lower()` is deliberately flat at the macro level:

```text
project_bundles
plan_sample_lowering
lower_connected_sample_groups
lower_vacant_sample_inputs
bind_subgraph_sample_inputs
lower_events
lower_detach
materialize_runtime_ports
materialize_executable_ir
complete_executable_ir // an internal authored-to-IR helper, not a phase
```

Each listed operation may call smaller helpers, but each temporary data type is
a plain nested struct with fields only—no methods. The lowering class owns the
functions, so all AST-to-IR work remains together and can be reordered or
regrouped without crossing representation boundaries.

The sample-lowering boundary is between semantic planning and topology
materialization. `plan_sample_lowering` groups authored connections by logical
target; the three consumers then have distinct responsibilities.

`SampleLoweringPlan` currently owns, per target logical sample port:

- the exact logical target; and
- references to its authored connection group.

Channel resolution, direct-routing eligibility, conversion planning, and
runtime-binding lookup still belong to connected materialization. They should
move into the plan only when more than one later decision can consume the
result, or when the move removes a graph-wide rediscovery.

The private phase products should be correspondingly narrow:

| Producer | Product consumed later |
|---|---|
| Bundle projection | boundary-to-subgraph map and the number of authored topology nodes |
| Connected sample lowering | bound concrete sample inputs and assigned subgraph sample outputs |
| Event lowering | materialized multi-member event output ports |

The lowered topology is a field on `GraphLowerer`, because it is authored
lowering scratch, not the externally meaningful IR. The resulting
`ExecutableGraphIR` is the only value passed to the next macro step.

The plan is computed from authored graph state. Connected lowering consumes it
to append direct edges or generated connection nodes and returns only the
bound-input and assigned-subgraph-output facts needed by later sample phases.
Vacant-input lowering then provides each unbound concrete input's default path.
Subgraph binding is last, after all materialized sample edges exist.

This preserves the final static execution representation while making it clear
where a future optimization should move work: into the plan, connected
materialization, vacancy handling, or subgraph binding—not into a monolithic
`lower_samples` routine.

## Contracts and tests

Behavior remains covered by focused constexpr graph tests in addition to final
graph tests. `GraphLowererTestAccess` is a test-only friend, not a public stage
API: it invokes the private production planner, connected-materialization,
vacancy, and subgraph-binding members and exposes only their plain phase facts.
The current tests cover plan grouping, direct native routing, connection-node
routing, fan-in, and vacant defaults.

| Unit | Contract examples |
|---|---|
| Bundle projection | concrete, tiled, and boundary ports map to expected topology endpoints |
| Sample plan | authored connections group by logical target |
| Connected materialization | native ordered channels are direct; reordered or fan-in routes create the expected connection nodes |
| Vacant-input lowering | each unbound concrete input receives one default path |
| Metadata indexing | disconnected, connected, and mixed ports retain their reported connectivity |

End-to-end builder tests remain the compatibility check. A later decision to
expose a unit for consteval profiling should add a narrow testable API at that
time, rather than freezing today's private workspace interface.

## Profiling model

There is intentionally no profiling capability or intermediate public API at
present. The named pass boundaries make later consteval benchmarks possible,
but that work must introduce a deliberate, narrow interface instead of
preserving this context as a de facto API.

## Migration rules

- `GraphBuilder` has no named lowering, metadata, or root-building façade. Do
  not add one; use `finish`, `GraphLowerer`, and `GraphCompiler` when the
  conversion boundary matters.
- Keep all authored-to-IR synthesis in `GraphLowerer`; in particular subgraph
  defaults, fan-in/fan-out, dangling-port completion, and scope construction
  must occur before executable IR is finished.
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
- Measure after each extraction once a deliberate profiling interface exists;
  do not infer performance from code shape.

## Completion criterion

The lowerer is an ordered private pipeline with phase-local state, existing
end-to-end graph tests preserve behavior, and `ExecutableGraphIR` remains the
only closed intermediate representation exposed between authoring and
compilation. `GraphCompiler` accepts no partially completed form.

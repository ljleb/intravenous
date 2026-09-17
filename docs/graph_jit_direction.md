# Graph JIT Direction

_Status: current design direction for synchronous whole-project graph compilation._

Related documents:

- [project_graph_application_architecture.md](./project_graph_application_architecture.md)
- [realtime_port_storage_planning.md](./realtime_port_storage_planning.md)
- [compiled_dsp_nodes.md](./compiled_dsp_nodes.md)
- [builder_lowering_pipeline_design.md](./builder_lowering_pipeline_design.md)
- [intravenous-llvm-hot-reload-and-whole-graph-design.md](./intravenous-llvm-hot-reload-and-whole-graph-design.md)
- [event_flows/README.md](./event_flows/README.md)

## Responsibility

`GraphJit` is an application module whose single responsibility is to
synchronously transform one complete configured project graph into one optimized
native executable graph generation.

It owns the whole-project LLVM/ORC compilation domain. It does **not** own:

- canonical desired project state;
- node-definition or node-instance caches;
- project connection declarations;
- live `NodeStorage`;
- active audio-pass state;
- safe-point activation/state migration policy.

Those responsibilities remain with `ProjectGraph`, `NodeDefinitions`,
`NodeInstances`, `GraphConnections`, and `GraphExecutor` respectively.

The application boundary should remain LLVM-free. Runtime request/result/event
types and `CompiledGraph` metadata belong in runtime-facing headers; the concrete
`GraphJit` application-module implementation may include LLVM/ORC privately in
its own target/header. A PImpl is not required merely to keep LLVM out of
`ProjectGraph` or event users when the app-module boundary already provides that
isolation.

### Current implementation checkpoint

The application/compiler shell around whole-graph lowering has landed. `GraphJit`
now captures one exact `ConfiguredGraph`/`NodeDefinitionsSnapshot` generation,
resolves registered concrete nodes to the accepted package revisions that created
them, parses those revisions' finalized O0 bitcode, indexes compiler callbacks by
`NodeCodeKey`, resolves symbolic configuration-pointer relocations to retained LLVM
globals, and owns a persistent project `LLJIT` with independently releasable
per-generation resources. Generated project LLVM is verified, optimized at O3,
and materialized synchronously before an immutable `CompiledGraph` is returned.

The lowering boundary is executable for the semantic identity case: an empty
project lowers to an empty canonical `NodeLayout` plus materialized no-op root
`tick_block`/`skip_block` operations and returns a real `CompiledGraph`. This
proves the complete lowering -> verification -> O3 -> ORC -> native-operation
path without introducing special runtime storage or lifecycle machinery.

The first deliberately narrow non-empty slice has also landed. A flat project
may contain several registered zero-port primitives, with no connections,
nested declarations, or auxiliary declaration-owned regions. Configured
`virtual_nodes` records are treated as source/introspection metadata over the
already-lowered concrete bundles and endpoints; GraphJit does not instantiate
them as executable runtime nodes.
Configuration pointer fields are reconstructed from symbolic retained-global
relocations: native pointer bytes are discarded during host planning, selected
immutable retained globals are deduplicated as package import roots, and final
node configuration globals contain LLVM-relocatable pointers plus byte addends
(or explicit null pointers). Each primitive invokes its exact accepted
native `declare_node` callback into the one canonical `NodeLayoutBuilder`.
`State` and `CompiledState` are ordinary canonical `NodeStorage` regions:
generated root operations materialize each reflected callback context from final
layout offsets and dispatch the selected package LLVM against those live bytes.
For this disconnected slice, root execution order is configured-bundle order;
connection-aware scheduling will replace that provisional order when explicit
schedule/SCC planning lands. Callback imports are grouped per package before a
compile-local package module is consumed once, and repeated uses of one package
callback share one imported root while still receiving distinct node
configuration/storage contexts. The reflected compiler callback ABI uses
explicit pointer/count span records rather than assuming an
implementation-specific `std::span` object representation. Root `skip_block`
is emitted only when every primitive in the execution sequence is block-skippable.
Unsupported shapes still fail explicitly at the lowering boundary; they are
never compiled as no-ops.

General graph lowering remains the deliberately isolated compiler work. The
current lowerer is intentionally split at a stable internal phase boundary:
`lowering_plan.cpp` performs host-side graph inventory/validation, canonical
declaration/layout planning, package callback-import planning, immutable node
configuration planning, and root execution planning; `lowering.cpp` only
realizes that completed plan into LLVM and consumes the selected compile-local
package modules. Planning must succeed before the output module is mutated or a
package module is consumed. This is the first anti-monolith landing site for the
remaining compiler work.

Primitive maximum-block splitting has now landed on the existing execution
plan. Each primitive execution step carries its accepted maximum block size, and
LLVM realization owns the single slicing loop used by both tick and skip: a root
block is partitioned into consecutive primitive-sized slices with the sample
index advanced by each slice. This keeps subdivision out of callback/configuration
planning and gives future port-context materialization one canonical per-slice
invocation boundary.

The second major landing-site refactor has now landed. Pure host-side connection
analysis lives in `graph_jit/connection_plan.{h,cpp}` and is intentionally usable
before LLVM/package realization. It inventories concrete nodes, classifies
connection access direction, derives sequential tick dependencies, computes
deterministic SCC/region ordering, records per-edge history/latency/conversion/
boundary/feedback facts, groups fanout by producer, derives the requirement
records consumed by the existing sample/event physical-storage choosers, and
emits semantic transient/persistent/external storage and liveness requests. A
compiled output never becomes a tick dependency merely because a realtime
consumer needs it: that edge remains classified for the later compiled-access
materialization phase. Realtime-to-realtime groups alone enter the realtime
storage chooser. Block-slice mismatches conservatively require materialization
until a later scheduler proves a shared subdivision. At the point this refactor landed, the lowering capability gate still rejected
ports/connections; the sample-edge slice below is the first consumer of this
analysis.

The first sample-edge realization has now landed, but with an important whole-
project-JIT-specific ABI: canonical `NodeStorage` contains only compiler-selected
sample backing, never `SharedPortData`, `InputPort`, or `OutputPort` objects.
GraphJit emits immutable per-node sample binding records containing final storage
offsets/capacities and passes the canonical storage base to the imported primitive
wrapper. The wrapper reconstructs short-lived node-API `InputPort`/`OutputPort`
values for that primitive invocation, anchored to the absolute sample index. Those
facades have no cross-call identity; after whole-project inlining/O3 they are
expected to scalarize into address/index arithmetic. The direct and transient
materialization choices therefore allocate only bounded sample backing. There is
no realtime heap allocation, lazy initialization, placement construction,
persistent façade cursor, or `SharedPortData` tax.

The existing `choose_sample_connection_implementation()` and
`choose_event_connection_implementation()` functions remain the physical-storage
policy boundary; GraphJit derives their requirement inputs and realizes their
returned choices rather than creating a competing policy layer. The old `Graph`
implementation is reference material only and must not constrain this runtime
representation. In particular, legacy fanout/cursor/storage objects should not be
carried forward merely to keep the old executor compiling.

The sample realization now has its own stable physical-plan layer in
`graph_jit/sample_physical_plan.{h,cpp}`. Primitive bindings refer to immutable
**sample representation handles**, not raw buffer identities or producer-group
storage objects. Every realtime producer group owns a canonical representation;
each realized
realtime connection resolves to a representation handle. Identity realtime
fanout branches resolve to that canonical representation. Point 8 adds explicit
derived transient representations for whole-port channel/layout conversions;
identical converted fanout branches share one derived representation and one
post-producer materialization operation without changing the primitive ABI.
Direct and transient-materialization representations are assigned exact byte ranges
inside one compile-time transient arena from their inclusive schedule live
intervals. The offline allocator tracks only currently-live ranges and places each
new representation in the lowest aligned free gap, so dead ranges can be split,
combined, and partially reused rather than leaving a historical whole-slot size
reserved. Equal-start allocations are considered size/alignment-first to reduce
fragmentation. Overlapping lifetimes never alias. The arena high-water mark and
all representation offsets are finalized before `NodeLayout` declaration and are
emitted as immutable primitive bindings; realtime execution only uses those
constant offsets and contains no allocator bookkeeping. The current packer is a
deterministic lowest-gap heuristic rather than a globally optimal interval-packing
solver; correctness and sub-range reuse are contractual, while globally minimal
arena size remains an optimization opportunity if measurements justify it. The
generic transient-arena planner is intentionally reusable by later event/workspace
planning.

The physical planner consumes the implementation decisions already made by
`choose_sample_connection_implementation()`; it does not choose policy again.
Only realtime branches receive realtime representation handles here; compiled
access branches remain unresolved for the later compiled-access executor. Whole-
port layout/channel-type conversion is represented by explicit derived branches
and generated materialization operations. Semantic channel projection and
permutation are represented by explicit sample compositions: source channels keep
their physical producer identity and independent read latency, complete sets of
per-channel target contributions normalize into canonical target order, and one
transient target-layout representation is gathered before the consumer. This does
not reintroduce connection helper nodes. Feedback and external representations
remain rejected at this checkpoint rather than being approximated with transient
storage.

### Realtime port realization rules

The remaining port work should preserve these invariants:

- **Canonical storage contains data, not API facades.** Persistent/transient sample
  payloads and genuinely required implementation state belong in `NodeStorage`;
  `InputPort`/`OutputPort` are invocation-local authored-node API adapters.
- **Bindings are immutable compiler facts.** Port implementation kind, offsets,
  capacities, layouts, history/latency parameters, and branch relationships should
  be emitted as immutable LLVM-visible records whenever possible. Only the
  `NodeStorage` base and current sample index/block size are dynamic.
- **No audio-thread setup.** Root execution performs no heap allocation, lazy
  initialization, ownership changes, or first-call construction. Any bounded
  invocation-local facade values are ordinary inline/stack/SSA values generated by
  the imported wrapper and are not lifecycle-managed runtime objects.
- **Producer groups own physical representations.** Fanout consumers reference one
  producer-group representation or explicit derived branches; there is no default
  one-buffer/one-object-per-edge model.
- **Conversions and fanout materialization are explicit execution steps.** A
  producer writes its canonical source-layout representation once. Identity branches
  share it; converted/remapped branches are planned materializations. Conversion is
  not hidden as mutable state inside `OutputPort`.
- **Transient and persistent state stay distinct semantically.** Current-block
  scratch may live in canonical allocation for bounded/reusable memory, but it is
  not migration state. Compact carry/rings/feedback are separate representations
  selected only when cross-kernel retention requires them.
- **Direct is allowed to have backing.** With the authored node API, producer output
  still needs an addressable current-block representation. `direct` means no extra
  connection copy/materialization between producer and consumer. A later fusion/SSA
  optimization may eliminate even that backing where profitable.

The shell continues to use the generated-root and canonical
`NodeLayout`/`NodeStorage` contract specified in this document: `CompiledGraph`
carries the finalized `NodeLayout` plus generated root `tick_block`/optional
`skip_block` operations, while lifecycle remains entirely in ordinary
`NodeStorage`. Whole-project lowering must not reintroduce a second node-storage
layout, a second lifecycle system, or a synthetic project-wide `access_block()`
merely to expose compiled outputs.

### Ordered implementation sequence

The implementation order is intentional. Each structural refactor should land
before the feature family that depends on it, so whole-graph lowering does not
accumulate one-off paths that must be disentangled later.
This is a hint, not a hard constraint. Use your own good judgement if ever in doubt.

1. **Stable node-lowering phases.** **Landed.** Separate host-side inventory/analysis,
   declaration/layout planning, package callback-import planning, configuration
   planning, execution planning, and LLVM realization without widening the
   accepted graph shapes.
2. **Multiple zero-port primitives.** **Landed.** Add deterministic execution order,
   multiple canonical declarations/configurations, callbacks from several
   packages, and several selected callbacks from one package. Derive root
   `skip_block` legality across the sequence.
3. **Configuration relocations.** **Landed.** Reconstruct configured pointer fields
   from already-resolved retained-global relocation records. Retained globals are
   imported as deduplicated package roots, native pointer bytes are zeroed during
   planning, and immutable node configuration LLVM contains symbolic pointers,
   byte addends, and explicit null slots rather than native process addresses.
4. **Primitive maximum-block splitting.** **Landed.** Primitive execution steps
   carry their accepted maximum block size, and one LLVM-emission path slices
   both tick and skip invocations while advancing sample indices correctly.
5. **Stable connection-analysis plans.** **Landed.** Pure host-side planning now
   records node/dependency topology, deterministic SCC/region scheduling,
   producer-group/connection temporal facts, sample/event chooser requirements,
   and semantic liveness/storage requests before any package LLVM is consumed.
   Realtime policy selection remains in `choose_*_connection_implementation()`;
   compiled and mixed-access directions are classified separately. Configured
   virtual-node records are metadata only: execution planning follows concrete
   bundles and configured connections directly and never lowers legacy/internal
   virtual/runtime helper nodes.
6. **Simple feed-forward sample connections.** **Landed.** Internal whole-port
   realtime sample edges realize `direct` and `transient_materialization` with
   bounded sample backing only. Immutable reflected binding records hold canonical
   storage offsets; imported primitive wrappers reconstruct invocation-local
   `InputPort`/`OutputPort` facades from the storage base and absolute sample index.
   No sample facade, cursor object, `SharedPortData`, or raw-region initializer is
   stored in `NodeStorage`.
7. **Stable sample-representation realization.** **Landed.** The point-6
   one-buffer-per-producer realization has been replaced by a producer-group
   physical plan with immutable representation handles, canonical producer
   representations, per-connection representation resolution, explicit transient
   lifetime semantics, and deterministic aligned byte-range packing in one
   transient arena. Dead ranges are reusable at sub-range granularity, including
   partial holes left by differently-sized representations. Primitive bindings no
   longer encode raw buffer identity. No retained
   representation is faked with transient storage; persistent/feedback/external
   kinds remain capability-gated for their dedicated later steps.
8. **Sample fanout, layout conversion, and channel composition.** **Landed for
   feed-forward realtime branches.** One canonical producer-layout representation
   is written once; identity consumers share it directly, while converted consumers
   bind explicit derived transient representations. Identical converted fanout
   branches are deduplicated to one representation/materialization. Semantic
   channel projection/permutation uses explicit composition materialization,
   including complete sets of separately configured target-channel contributions;
   each source channel retains its producer storage and independent read latency.
   Materialization is generated whole-project LLVM using absolute-indexed physical
   storage and contains no runtime converter object, heap allocation, or
   `OutputPort` conversion state.
9. **Sample history and latency.** **Landed for declared realtime sample history/latency.**
   `compact_persistent_carry` uses one transient absolute-indexed working ring plus
   exactly the retained tail in persistent raw `NodeStorage`; the tail is restored
   before its producer and committed after producer-side materializations. Larger
   retention uses `persistent_ring`, binding primitives directly to one power-of-two
   persistent ring. Immutable input bindings carry authored history/read latency,
   converted branches materialize the complete historical read window, and both
   modes use absolute sample-index addressing. Persistent compiler-owned raw regions
   carry stable migration identities and exact-shape `NodeStorage` migration copies
   their bytes across generations; transient arenas never migrate. This point covers
   declared output latency and input/output history. Feed-forward whole-graph
   path-latency equalization is also landed: cumulative node/internal/output latency
   propagates through the schedule, faster branches receive compiler-owned read
   compensation, and that timing survives conversion, fanout, channel composition,
   history, and target-channel projection/permutation. Feedback/SCC latency remains
   deferred to point 12.
10. **Event-port realization refactor and simple event flow.** **Landed for direct, transient block adaptation, conversion, and feed-forward fanout.**
    Events use immutable compiler bindings over bounded raw `NodeStorage`; imported
    primitive wrappers reconstruct invocation-local event facades rather than
    persisting `EventSharedPortData`/port objects. Exact-type, zero-retention,
    unsliced realtime producer groups realize `direct` bounded sequences. Sliced
    producers/consumers realize `transient_sequence`: the producer sequence is
    cleared once per root invocation, producer slices append into it, and a
    consumer-facing sequence is materialized after the complete producer step.
    Event conversion plans are preserved by semantic analysis and realized as
    explicit transient sequence operations; identical converted fanout branches
    share one derived representation/materialization.
11. **Event retention.** Add compact event carry, persistent rings, retained-window
    filtering, bounded capacity/workspace policy, and liveness reuse.
12. **SCC/feedback execution.** Turn existing SCC analysis into feedback-aware
    scheduling, `feedback_ring` realization for sample/event groups, and nonzero
    reflected `scc_feedback_latency`.
13. **External project boundaries.** Define the generated root boundary-binding ABI
    and realize `external_boundary` without copying through compatibility Graph
    objects. Keep boundary ownership outside `CompiledGraph` storage.
14. **Remaining declaration/runtime semantics.** Add nested declarations,
    declaration-owned auxiliary/shared-array regions, activity/TTL, deferred
    detach, and generalized skip semantics through existing plans rather than side
    paths.
15. **Compiled DSP access.** Add internal endpoint metadata, compiled-access
    component plans/executors, batching, and bounded workspaces after realtime
    connection storage is stable.
16. **GraphExecutor integration.** Add active/pending generations, canonical
    `NodeStorage` construction/migration, safe-point activation, root execution,
    boundary binding, and compiled-access dispatch. `CompiledGraph` remains
    independently testable before this point.
17. **Optimization refinements.** Verify generated hot-path assembly and then improve
    liveness reuse, storage cost choices, fusion/SSA direct forwarding, vectorization,
    and target-specific optimization only after the semantic compiler surface is
    complete.

The root-build transaction remains:

```text
ProjectGraph
    |
    +-- NodeInstances
    |      configure/embed one complete instance batch
    |
    +-- GraphConnections
    |      apply all cross-instance logical connections
    |
    | finish root ConfiguredGraph
    |
    +-- GraphJit
    |      synchronously compile one CompiledGraph
    |
    +-- GraphExecutor
           accept the compiled successor generation
```

`GraphJit` and `GraphExecutor` are sibling children of `ProjectGraph`. `GraphJit`
never publishes directly into `GraphExecutor` during the same cause.

## Two sequential JIT stages, two ownership domains

The application intentionally has two JIT stages with different purposes.

The existing package/configuration JIT is analogous to a compile-time
metaprogram JIT:

```text
package C++ / retained package LLVM
        |
        v
package/configuration JIT
        |
        | execute registered construction/configuration callbacks
        v
ConfiguredGraph structures and configured values
```

The project graph JIT consumes the resulting graph description and the retained
primitive implementation LLVM:

```text
ConfiguredGraph
+ exact provider/code provenance
        |
        v
whole-project lowering and explicit graph analyses
        |
        v
specialized root-node LLVM + compiled-access executors
        |
        v
GraphJit ORC
        |
        v
native CompiledGraph
```

The first JIT executes configuration code. The second JIT compiles the DSP
program described by that configuration.

The native DSP output of the first JIT is **not** the preferred code-generation
input to the second JIT. `GraphJit` imports the required callback closures from
the retained primitive node LLVM associated with the exact providers used by the
configured graph so whole-project inlining and optimization remain possible. The
accepted native node declaration/lifecycle callbacks remain useful for the canonical
`NodeLayout`/`NodeStorage` contract and must stay pinned by the exact accepted
package revisions.

The two JITs may share non-app-module LLVM utility code, target setup, optimizer
helpers, object-cache helpers, or memory-manager helpers. They should not share
application-module ownership merely because both use ORC.

## Project-JIT ownership

`PackageJit` owns the persistent `ModuleLoader` and therefore the shared
package/configuration ORC state. A shared package `LLJIT` survives individual
package revisions, while each accepted `PackageRevision` pins its
revision-specific package code/resources and finalized compiler artifact.

The whole-project graph JIT uses the analogous lifetime pattern in its own
domain:

```text
GraphJit
    |
    v
shared project LLJIT
    |
    +-- project generation N JITDylib/resources
    +-- project generation N+1 JITDylib/resources
    +-- project generation N+2 JITDylib/resources
```

A single `LLJIT` owned by `GraphJit` may live for the application lifetime. Each
compiled project generation has independently releasable ORC resources through a
generation-specific `JITDylib`/`ResourceTracker` lifetime object.

Old and new generations must be able to coexist while `GraphExecutor` finishes a
pass, prepares `NodeStorage` migration, or retains active/pending generations.
A `CompiledGraph` therefore pins its code/resource lifetime rather than exposing
naked function pointers whose lifetime is implicit.

## Synchronous compilation is the intended contract

Whole-project graph compilation is intentionally synchronous inside one
`ProjectGraph` rebuild transaction.

The target is for it to be fast enough that introducing an asynchronous compiler
pipeline would add more consistency/lifetime machinery than value. The compiler
should do graph-specific work before LLVM so the final LLVM module is already
close to the desired native program.

Conceptually:

```cpp
GraphJitCompileResult GraphJit::compile(GraphJitCompileRequest const& request);
```

and the caller immediately continues with the returned result in the same
propagation cause.

If whole-project compilation later becomes unexpectedly expensive, treat that
as a compiler-performance problem to measure first. Do not pre-commit the
architecture to asynchronous compilation merely to hide avoidable compiler
work.

Package discovery/build/reload may still be asynchronous because that is a
different source-build domain. The synchronous rule here applies specifically
to the final configured project graph -> native graph generation step.

## Exact-generation consistency

`GraphJit` must compile exactly the provider/code generations used to construct
the supplied `ConfiguredGraph`.

It must **not** ask `NodeDefinitions` for whatever definitions happen to be
current when compilation begins. Doing so would both create an unwanted
application-module dependency and make one project transaction vulnerable to a
mixed definition generation.

The compile input therefore carries the same immutable definition generation
used to construct the graph, and configured registered nodes carry exact
provider/code identity. Each accepted `PackageRevision` pins both its native
callbacks/lifetime and the finalized O0 bitcode used by the project JIT.

The invariant is:

> One root graph is configured, declared, and compiled against one coherent
> definition world.

## The compiled project masquerades as one root node

The optimized project graph should use the existing root-node execution model
instead of defining a parallel project-kernel object model.

The generated project root has no public inputs or outputs:

```text
project root
    inputs  = {}
    outputs = {}
```

This matches the existing `BlockNodeExecutor` root contract. The optimized root
keeps ordinary node semantics, but declaration is consumed as a compile-time
layout contract rather than materialized as a runtime JIT entrypoint. Runtime
behavior is therefore:

```text
compile time: declare/layout planning
runtime:      NodeStorage initialize()/move()/release()
runtime:      generated tick_block()
runtime:      generated skip_block() when legal
```

The constituent nodes retain their own declaration and lifecycle semantics.
Declaration is performed during lowering, before final LLVM emission: the compiler
invokes each exact accepted native `declare_node` callback into one
`NodeLayoutBuilder`, adds compiler-owned raw regions, then finalizes the
`NodeLayout`. This preserves each node's state structures, lifecycle callbacks,
dependencies, and nested-state relationships while making every final storage
offset available as a constant to LLVM. `NodeStorage` remains responsible for
invoking initialization, move/migration, release, and destruction in the order
described by that layout. Do not replace that orchestration with monolithic
generated project initialize/move/release functions.

The root node has no compiled output ports, therefore it has no
`access_block[_batch]()` operation. Compiled outputs inside the project remain
addressable through immutable `CompiledGraph` metadata described below; they are
not exposed by pretending that the zero-port project root has synthetic outputs.

## One canonical `NodeLayout` and one `NodeStorage`

There is exactly one runtime storage allocation model for an executable graph
generation: the existing `NodeLayout`/`NodeStorage` machinery.

`GraphJit` must not introduce `CompiledGraphNodeStorageLayout`,
`GraphKernelStorage`, or another parallel state arena. Lowering populates one
`NodeLayoutBuilder` and finalizes it before emitting final storage accesses into
LLVM. The completed `NodeLayout` becomes part of `CompiledGraph`, and
`GraphExecutor` creates and owns the corresponding `NodeStorage`.

The canonical storage covers `CompiledState` as well as normal `State`. The
ordinary declaration/lifecycle machinery represents both state domains and makes
the same `CompiledState` object available to `tick_block()` and compiled-access
callbacks. `initialize()`, `move()`, and `release()` semantics apply to both where
the node defines them.

Source introspection publishes symmetric metadata for `State` and
`CompiledState`: a Clang nominal type identity (USR), a definition fingerprint,
size/alignment, and reflected field layout. That exact definition identity is the
cross-package-generation compatibility boundary for typed state migration. A
same-process type token remains sufficient when both generations use the exact
same loaded C++ type, but equal RTTI names or equal byte size alone are not a
safe hot-reload migration contract.

All project-owned memory whose lifetime can cross an execution call or be reused
between calls should normally be allocated through the same `NodeLayout` and
stored in the same `NodeStorage`, including for example:

- node `State` and `CompiledState`;
- history/latency/feedback carry;
- persistent event storage;
- root/compiler-owned activity state;
- bounded reusable compiled-access workspaces;
- one statically packed transient arena with compile-time byte ranges selected by liveness analysis;
- other fixed-size compiler-selected project regions.

This gives the whole-project compiler control over physical declaration order.
The current layout builder packs regions in declaration order while solving
`initialize_order` separately from dependency information, so lowering can
co-locate data in approximately the order generated O3 code will access it
without conflating physical locality with lifecycle ordering.

### No compiler-owned façade initialization path

Compiler-owned raw regions are bytes with semantic storage meaning, not a place
to persist C++ port façade objects. GraphJit should prefer immutable LLVM binding
records plus constant NodeStorage offsets over raw-region initializers or pointer
fixup passes. The final storage base is supplied to generated root code, and the
imported node wrapper derives invocation-local API views from that base.

If a future physical representation genuinely requires nontrivial persistent
runtime state, it should be modeled explicitly in the connection/storage plan and
given ordinary bounded storage/lifecycle semantics. Do not add a generic
first-call or pre-audio façade-construction mechanism merely because the old Graph
represented connections as mutable C++ objects.

### Compiler-owned raw regions

The authored `DeclarationContext::local_array()` API associates an allocation
with a typed `std::span` field in node `State`. Generated project code should not
be forced to manufacture a C++ state field merely to reserve a compiler-private
region whose address is known by constant offset.

`NodeLayoutBuilder` should therefore gain a low-level aligned-region declaration
primitive suitable for compiler-generated root storage. It should produce an
ordinary `NodeLayout::Region` in the same layout and allocation as every other
node region. Generated LLVM can then address the resulting storage by constant
offset after declaration/layout is complete, while `local_array()` remains the
typed authored convenience API.

This is an extension of `NodeLayout`, not a second storage system.

Truly request-sized caller input/output objects need not be embedded in
`NodeStorage`; their size may not be bounded at graph-compilation time. But if a
workspace has a known maximum size or is intentionally reusable across queries,
the compiler should prefer a root-owned `NodeLayout` region rather than a
separate project scratch allocation.

## Compiled access is internal to the generated project

A node with at least one compiled **output** must remain requestable through its
normalized compiled-access operation. The project root itself is not such a
node, so compiled access is represented separately from the root-node interface.

`CompiledGraph` should carry an immutable index from requestable internal
compiled output ports to compiler-generated access executors. Conceptually:

```text
(node bundle, compiled output port)
        |
        v
compiled-access component + sink ordinal
        |
        v
specialized generated component executor
```

The exact host ABI is implementation work, but it should expose internal
compiled outputs without inventing project-root output ports.

### Static topology planning belongs in lowering

Most compiled-access graph structure is static and should be specialized by the
lowerer rather than rediscovered for every query. At minimum lowering can
precompute:

- which nodes/ports participate in compiled access;
- connected components/subgraphs formed by compiled-port edges;
- the mapping from requestable compiled outputs to their component/sink ordinal;
- reverse dependency order for demand propagation;
- forward topological evaluation order;
- fanout/convergence structure;
- constant port/node/state offsets and callback targets;
- which nodes have trivial/no-op propagation;
- fixed-capacity request-set/workspace storage where useful.

The dynamic part of a query is primarily the requested sample grids/event
intervals and the resulting request-set contents, not discovery of graph
adjacency.

### One query batches all requested sinks before execution

A caller may request compiled outputs from any number of internal nodes in one
logical operation; there is no small fixed node-count limit. Requests should be
grouped by their precomputed compiled-access component. Disconnected components
may execute independently because they cannot share upstream compiled work.

Within one component the semantic order is fixed:

```text
seed all requested sink outputs for this query
        |
        v
reverse planning in precomputed reverse order
        |
        | union/coalesce requests at converging ports
        v
complete component demand
        |
        v
forward evaluation in precomputed topological order
        |
        v
return requested sink results
```

A node receives the complete accumulated request sets for all of its requested
compiled outputs when its propagation/access callback runs. Whenever topology
permits, each implicated node participates once in reverse planning and once in
forward execution for the complete component query, rather than once per sink
or downstream path.

The lowerer may inline and specialize these propagation/access callbacks so the
runtime executor manipulates request sets, not generic graph data structures.

## Lowering boundary

The hard compiler seam remains one operation that receives the complete
configured graph plus exact resolved primitive implementation information and
populates one caller-owned LLVM module.

Its inputs include conceptually:

```text
ConfiguredGraph
kernel specialization
exact retained package modules
resolved primitive tick/skip/access/propagation LLVM callbacks
exact accepted declaration/lifecycle metadata/callbacks
resolved configuration-pointer relocations
```

It must not query `PackageJit`, `ModuleLoader`, live `NodeDefinitions`, or any
current registry state.

The lowerer produces:

```text
canonical finalized NodeLayout
specialized project-root node LLVM
compiled-access component executor LLVM
immutable LLVM globals/tables needed by those programs
host metadata naming the generated root/component symbols and internal endpoints
```

It does **not** return a parallel storage plan. Canonical declaration is an early
phase *inside* lowering: accepted native declaration callbacks and compiler raw
regions are committed to `NodeLayoutBuilder`, `build()` fixes every offset, and
only then does final LLVM generation encode those offsets. ORC materialization
therefore consumes LLVM whose storage addresses already agree exactly with the
`NodeLayout` returned alongside it.

Source package LLVM modules and temporary `llvm::Function*`/`GlobalVariable*`
anchors are valid only during lowering. Anything needed after lowering must have
become generated/imported LLVM, immutable host metadata, or canonical
`NodeLayout` information.

## `CompiledGraph`

The result of `GraphJit` is one immutable executable generation. Conceptually it
contains:

```text
project/rebuild + definition-generation provenance
exact participating PackageRevision pins
specialized zero-input/zero-output root node operations
canonical NodeLayout
compiled-access endpoint index
specialized compiled-access component entrypoints/metadata
ORC code/resource lifetime handle
debug/execution-plan metadata
```

It does not own mutable `NodeStorage` and does not define a second state-layout
representation. It also does not need a synthetic project-wide
`access_block()`; compiled-output requests are routed through the internal
endpoint/component index.

`CompiledGraph` owns code, layout, and immutable planning metadata.
`GraphExecutor` owns mutable storage and active execution state.

## `GraphExecutor` boundary

`GraphExecutor` receives the complete `CompiledGraph` after `GraphJit` returns.
It owns:

- active and pending executable generations;
- one live `NodeStorage` per retained executable generation;
- state/`CompiledState` initialization, migration/move, release, and destruction
  through the canonical layout/lifecycle machinery;
- pass-scoped execution state/resources;
- sequential root-node execution requests;
- compiled sample/event requests routed to the active generation's internal
  compiled-access components;
- safe-point activation.

Receiving a new generation does not mutate an in-progress audio pass. Expensive
work that is safe outside the pass boundary may be prepared immediately, but the
active generation changes only after the current pass completes.

This keeps the useful replacement invariant from the old task-runner mechanism
without retaining the old task graph.

## Failure semantics

A project mutation may successfully update desired/configured project state and
still fail during `GraphJit` compilation.

In that case:

- `ProjectGraph` retains the desired revision and compile diagnostics;
- `GraphExecutor` is not given the failed generation;
- the previous complete executable generation may continue running;
- a later project/definition change retries the entire root-build transaction.

Desired graph revision and active executable revision are therefore distinct
state even though compilation itself is synchronous.

## Compiler pipeline

`GraphJit` should orchestrate compiler phases, but deterministic graph analyses
and heuristics belong in ordinary testable compiler functions rather than in ORC
callbacks.

A target pipeline is:

```text
ConfiguredGraph
    |
    v
logical whole-graph lowering
    |
    v
dependency / schedule / SCC / region analysis
    |
    +--> compiled-port component + reverse/forward order analysis
    |
    v
history / latency / event-window analysis
    |
    v
connection implementation + liveness/reuse planning
    |
    v
canonical declaration/layout planning
    |
    +--> accepted native declare_node callbacks
    +--> compiler-owned raw regions
    +--> finalized NodeLayout + constant offsets
    |
    v
specialized project-root + compiled-access LLVM generation
    |
    v
graph-specific optimization / -O3 / target optimization
    |
    v
ORC materialization
    |
    v
resolve generated root/component operations
    |
    v
CompiledGraph + finalized NodeLayout
```

Physical node state, persistent project state, and reusable compiler-selected
regions all become one `NodeLayout`/`NodeStorage`. Pure storage analyses may
still decide which logical values need regions, their size/alignment, liveness,
and desirable declaration order before LLVM/declaration generation; they do not
create a parallel runtime allocation model.

See [realtime_port_storage_planning.md](./realtime_port_storage_planning.md) for
the rule that logical connections do not imply buffers, and
[compiled_dsp_nodes.md](./compiled_dsp_nodes.md) for the globally batched
compiled-access semantics that lowering specializes.

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

The deliberately isolated missing implementation is
`graph_jit::lower_configured_graph_to_llvm`. The shell now uses the generated-root
and canonical `NodeLayout`/`NodeStorage` contract specified in this document:
`CompiledGraph` carries the finalized `NodeLayout` plus generated root
`tick_block`/optional `skip_block` operations, while lifecycle remains entirely
in ordinary `NodeStorage`. Whole-project lowering must not reintroduce a second
node-storage layout, a second lifecycle system, or a synthetic project-wide
`access_block()` merely to expose compiled outputs.

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
input to the second JIT. `GraphJit` imports/clones the retained primitive node
LLVM associated with the exact providers used by the configured graph so
whole-project inlining and optimization remain possible. The accepted native
node declaration/lifecycle callbacks remain useful for the canonical
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
- statically sized reusable transient slots selected by liveness analysis;
- other fixed-size compiler-selected project regions.

This gives the whole-project compiler control over physical declaration order.
The current layout builder packs regions in declaration order while solving
`initialize_order` separately from dependency information, so lowering can
co-locate data in approximately the order generated O3 code will access it
without conflating physical locality with lifecycle ordering.

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

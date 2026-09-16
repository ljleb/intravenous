# Graph JIT Direction

_Status: current design direction for synchronous whole-project graph compilation._

Related documents:

- [project_graph_application_architecture.md](./project_graph_application_architecture.md)
- [realtime_port_storage_planning.md](./realtime_port_storage_planning.md)
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

The root-build transaction is therefore:

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
specialized project LLVM
        |
        v
GraphJit ORC
        |
        v
native CompiledGraph
```

The first JIT executes configuration code. The second JIT compiles the DSP
program described by that configuration.

The native output of the first JIT is **not** the preferred code-generation
input to the second JIT. `GraphJit` should import/link the retained primitive
node LLVM associated with the exact providers used by the configured graph so
whole-project inlining and optimization remain possible.

The two JITs may share non-app-module LLVM utility code, target setup, optimizer
helpers, object-cache helpers, or memory-manager helpers. They should not share
application-module ownership merely because both use ORC.

## Current JIT ownership and future graph-JIT ownership

Today the package/configuration ORC state is owned below `ModuleLoader`, which is
kept alive by package reload state. A shared package `LLJIT` survives individual
package revisions, and each loaded package revision owns generation-specific ORC
resources such as a `JITDylib`/resource tracker.

The whole-project graph JIT should use the analogous lifetime pattern in its own
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
compiled project generation should have independently releasable ORC resources,
preferably through a generation-specific `JITDylib` and/or `ResourceTracker`.

Old and new generations must be able to coexist while `GraphExecutor` finishes a
pass, prepares state migration, or retains an active/pending generation.

A compiled generation must therefore own/pin its JIT resources through an RAII
object rather than hand `GraphExecutor` naked function pointers whose lifetime
is implicit.

## Synchronous compilation is the intended contract

Whole-project graph compilation is intentionally synchronous inside one
`ProjectGraph` rebuild transaction.

The target is for it to be fast enough that introducing an asynchronous compiler
pipeline would add more consistency/lifetime machinery than value. The compiler
should do graph-specific work before LLVM so the final LLVM module is already
close to the desired native program.

The synchronous call is conceptually:

```cpp
CompiledGraph GraphJit::compile(GraphJitRequest const& request);
```

and the caller immediately continues with:

```text
CompiledGraph result = GraphJit(...)
GraphExecutor(result)
```

within the same propagation cause.

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

The compile input should therefore carry or retain exact provider/code
provenance. Initially that may mean passing the same immutable
`NodeDefinitionsSnapshot` used by `NodeInstances`; eventually the configured
node instances/graph may pin sufficient provider LLVM/module references directly
so the whole registry snapshot is unnecessary.

The invariant is:

> One root graph is configured and compiled against one coherent definition
> world.

## `CompiledGraph`

The result of `GraphJit` is an immutable executable generation. Its exact ABI is
implementation work, but conceptually it contains:

```text
project/rebuild revision provenance
native root graph function interface
NodeStorage layout requirements
state/lifecycle metadata
state-correspondence/migration metadata
compiled sample/event access entry points as applicable
ORC code/resource lifetime handle
debug/execution-plan metadata
```

The native root object must support the execution modes required by ordinary DSP
nodes, including sequential realtime execution and compiled sample/event access.
The execution ABI should continue using node concepts rather than create a
parallel lane/task vocabulary.

`CompiledGraph` owns code and immutable planning metadata. `GraphExecutor` owns
mutable storage and active execution state.

## `GraphExecutor` boundary

`GraphExecutor` receives the complete `CompiledGraph` after `GraphJit` returns.
It owns:

- active and pending executable generations;
- live `NodeStorage`;
- state initialization/release/migration;
- pass-scoped execution state/resources;
- sequential execution requests;
- compiled sample/event requests against the active generation;
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
    v
history / latency / event-window analysis
    |
    v
connection implementation planning
    |
    v
transient liveness + scratch reuse
    |
    v
persistent NodeStorage + lifecycle planning
    |
    v
specialized whole-project LLVM generation
    |
    v
graph-specific optimization / -O3 / target optimization
    |
    v
ORC materialization
    |
    v
CompiledGraph
```

See [realtime_port_storage_planning.md](./realtime_port_storage_planning.md) for
the rule that logical connections do not imply buffers and for the pure
connection implementation planner required before LLVM generation.

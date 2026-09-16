# Event Flow: Package Reload

This procedure starts when package compilation/reload work has completed and a
new provider batch is ready to be applied. Raw filesystem notifications and
asynchronous package source/build scheduling may occur earlier and are not part of this
synchronous propagation.

## Propagation tree

```mermaid
flowchart TD
    SRC["Package reload completion"]
    PR["PackageReload"]
    ND["NodeDefinitions"]
    PG["ProjectGraph"]
    NI["NodeInstances"]
    GC["GraphConnections"]
    GJ["GraphJit"]
    GE["GraphExecutor"]

    SRC --> PR
    PR -->|"completed provider replacements / removals"| ND
    ND -->|"one immutable versioned NodeDefinitionsSnapshot"| PG
    PG -->|"1. reconfigure complete requested node batch against exactly this snapshot"| NI
    PG -->|"2. re-resolve complete connection batch against new embeddings"| GC
    PG -->|"3. successor ConfiguredGraph ⇄ synchronous CompiledGraph"| GJ
    PG -->|"4. compiled successor generation"| GE
```

## Data movement

`PackageReload` owns reload/build completion and publishes one
`PackageReloadResults` batch containing successful packages, module-definition
candidates, leaf-definition candidates, and failures. Its public result/value
contracts live in type-only headers, so `PackageReload` and `NodeDefinitions`
remain independently constructed app modules connected only by their explicit
linker-event bridge.

`NodeDefinitions` reconciles those changes into a complete immutable registry
snapshot.

`ProjectGraph` stores that snapshot as the current definition world and starts
one root rebuild using its unchanged desired project graph state.

The snapshot is passed once to `NodeInstances`. `NodeInstances` may internally
preserve configured cache entries whose provider/version dependencies are
unchanged and rebuild only affected cache entries, but all recursive definition
resolution for this invocation uses this exact snapshot.

No nested configuration call re-enters `NodeDefinitions`.

`GraphConnections` is invoked only after the entire requested instance set has
been embedded. Connections whose path no longer resolves become dangling; they
are retained so that the same stable path can resolve again in a later reload.

The resulting complete `ConfiguredGraph` is synchronously compiled exactly once
by `GraphJit` using the same provider/code generation, then the resulting
`CompiledGraph` is submitted once to `GraphExecutor`.

## Async source separation

Filesystem discovery/change detection and expensive package compilation should
not keep one application propagation open. Completion of background work starts
this new propagation as a new cause.

This separation prevents flows such as project replay -> package compilation ->
project graph rebuild from re-entering a module already reached by the project
replay cause.

## Derived read models and notifications

If `NodeSourceIntrospection` needs to observe this change, `ProjectGraph` should
update it once using the combined state/result already available in the root-build
transaction. Do not preserve separate definition-side and instance-side paths
that converge on the read model.

Any asynchronous UI notification to `SocketRpcServer` that would re-enter an
incoming RPC/reload propagation must be published only after the current cause
unwinds, as a new cause.

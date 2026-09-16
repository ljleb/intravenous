# Startup And Project-Graph Realization Order

_Status: current startup direction for the generalized project graph._

The detailed propagation procedures are in
[event_flows/startup_and_project_replay.md](./event_flows/startup_and_project_replay.md).

## Core distinction

Startup must keep three states separate:

- **desired project state** — durable node-instance declarations owned by
  `NodeInstances` and project-connection declarations owned by
  `GraphConnections`;
- **configured project graph** — the root `ConfiguredGraph` that can currently
  be built from available definitions;
- **compiled project graph** — the immutable `CompiledGraph` synchronously
  produced by `GraphJit` from that configured generation;
- **active execution** — the executable generation/current mutable storage owned
  by `GraphExecutor`.

Project load reconstructs desired state. It does not require every IV package to
be compiled first, and it does not require audio execution to be active.

## Construction order

1. Construct all long-lived app modules.
2. Bind every bridge/event connection before starting sources that can emit
   project-domain activity.
3. Start only inert/local infrastructure needed by replay and package discovery.
4. Load and parse project persistence synchronously into normalized batches.
5. Replay normalized desired-instance and desired-connection batches through
   `ProjectGraph`; it forwards those batches once to `NodeInstances` and
   `GraphConnections`, which own the desired state.
6. `ProjectGraph` performs one root-build transaction with the definitions
   snapshot currently available, which may be empty/incomplete.
7. Finish server-ready initialization and enable autosave only after replay is
   complete.
8. Start `PackageWatcher` and perform initial event-driven package discovery.
9. Each discovered/changed package batch starts the normal package transaction:
   `PackageWatcher -> {PackageJit, PackageDefinitions -> NodeDefinitions -> ProjectGraph}`,
   with `PackageJit` invoked first and its complete result returned to
   `PackageWatcher` before `PackageDefinitions` is entered.
10. `ProjectGraph` orchestrates reconstruction using the desired instances owned
    by `NodeInstances` and desired connections owned by `GraphConnections` against
    the new immutable definitions snapshot, synchronously compiles the resulting
    root graph through `GraphJit`, and submits the compiled successor to
    `GraphExecutor`.

## Valid initialized state before package realization

After project replay and before package build completion, all of these are
valid:

- requested node instances remain owned by `NodeInstances` even when some
  definitions are unavailable;
- their C++ configuration argument-list source is retained even if it cannot yet
  be compiled;
- project-wide `ProjectNodePortMatcher` connections are retained even when they
  currently match nothing;
- `NodeInstances` reports unresolved/failed configured instances rather than
  deleting requested state;
- `GraphConnections` reports dangling unresolved matchers rather than deleting
  requested connections;
- the current root `ConfiguredGraph` may therefore be partial or empty;
- `GraphExecutor` may have no active generation or may run the latest complete
  generation available under the chosen execution policy.

## Package filesystem activity starts its own package transaction

Project replay does not synchronously reach into package watching/building.
`PackageWatcher` activity is a separate source invocation after the watcher has
started.

Within one watcher-originated package transaction, `PackageWatcher` may
synchronously invoke `PackageJit` once for the complete build subset, update its
own dependency watches from the returned result, then pass the complete package
update once to `PackageDefinitions`. The resulting accepted package revision
snapshot flows once through `NodeDefinitions` and then into `ProjectGraph`.

This keeps startup replay and package-source causes separate while still allowing
one package refresh transaction to be synchronous and tree-shaped.

## One snapshot per instance batch

When `ProjectGraph` rebuilds because definitions changed, it passes exactly one
immutable `NodeDefinitionsSnapshot` to the single `NodeInstances` invocation for
that transaction.

Every top-level requested node and every nested `g.node(...)` lookup/configure
operation uses that same snapshot. A concurrently arriving definition snapshot
can only become input to a later root-build transaction.

## Graph compilation and execution activation

Whole-project `GraphJit` compilation is synchronous inside the `ProjectGraph`
rebuild transaction. It uses exactly the configured graph/provider generation
produced by that transaction and returns one immutable `CompiledGraph` before
`ProjectGraph` calls `GraphExecutor`.

Receiving that compiled generation does not mutate an active audio pass. The
active generation remains immutable for the duration of a complete pass, and a
completed successor can replace it only at a safe boundary after the current
pass finishes.

# Startup And Project-Graph Realization Order

_Status: current startup direction for the generalized project graph._

The detailed propagation procedures are in
[event_flows/startup_and_project_replay.md](./event_flows/startup_and_project_replay.md).

## Core distinction

Startup must keep these states separate:

- **desired project state** — durable node-instance declarations owned by
  `NodeInstances` and project-connection declarations owned by
  `GraphConnections`;
- **configured project graph** — the root `ConfiguredGraph` that can currently
  be built from available definitions;
- **compiled project graph** — the immutable `CompiledGraph` synchronously
  produced by `GraphJit` from that configured generation;
- **prepared/active execution generation** — one complete `ExecutionGeneration` owned
  by the `GraphExecutor` app module, with realtime mutable storage owned internally by
  `RealtimeExecutor`; and
- **background execution/persisted state** — background mutable evaluation state plus
  immutable persisted-state versions owned internally by `BackgroundExecutor`.

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
10. `ProjectGraph` reconstructs using the desired instances owned by
    `NodeInstances` and desired connections owned by `GraphConnections` against
    one immutable definitions snapshot. It commits and publishes valid configured
    structure immediately, then queues executable compilation on its serialized
    worker. The worker pins configuration/provenance, discards unstaged obsolete
    work, and prepares the successor outside the project edit lock before guarded
    publication to `GraphExecutor`.

## Valid initialized state before package realization

After project replay and before package build completion, all of these are
valid:

- requested node instances remain owned by `NodeInstances` even when some
  definitions are unavailable;
- their restricted configuration argument-list source is retained even if it cannot yet
  be compiled;
- project-wide `ProjectNodePortMatcher` connections are retained even when they
  currently match nothing;
- `NodeInstances` reports unresolved/failed configured instances rather than
  deleting requested state;
- `GraphConnections` reports dangling unresolved matchers rather than deleting
  requested connections;
- the current root `ConfiguredGraph` may therefore be partial or empty;
- `GraphExecutor` may have no active execution generation or may run the latest
  complete generation available under the chosen execution policy;
- its internal `BackgroundExecutor` may independently have no pending work yet or
  retain the latest coherent background/persisted state.

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

Whole-project compilation runs on one serialized `ProjectGraph` worker after
configuration commits. `GraphJit::compile()` is synchronous within that worker
cause and consumes pinned immutable configuration/provenance. The compiler and
executor preparation do not hold the project edit lock. The current target is
checked again before a prepared successor becomes an accepted chain link.
Compiler failure retains the new authoritative configuration and the previous
executable. Observation/control runtime identity must distinguish those states.
The previous contract is preserved in
[the historical startup snapshot](historical/startup_realization_order_before_async_configured_publication_2026-11-16.md).

`GraphExecutor` prepares one complete `ExecutionGeneration`: both internal
actor realizations, generation-specific routes/endpoints, producer reserves,
background selection descriptors and migration metadata. Its move-only
preparation handle either publishes the complete generation once or discards it
off-thread. Publication is short and follows the existing ordered chain;
resources are ready before the generation can become pending.

Staging does not mutate an active realtime pass or a workload already selected by the
background worker. At a later legal realtime pass boundary, the internal realtime actor
publishes all final old-generation chains, synchronously publishes the already-prepared
successor `ExecutionGeneration*` to the internal background actor, and then swaps its
active realtime generation. The handoff only exposes the prepared transition to the
worker; it does not allocate or execute background work synchronously. Background may lag, finish
old-generation selected work, drain the now-closed old-generation queues, and perform
the prepared migration before interpreting new-generation inputs. These actor handoffs
are internal to `GraphExecutor`, not app-module event propagation.

See
[realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md)
for the normative hot-reload protocol.

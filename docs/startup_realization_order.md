# Startup And Project-Graph Realization Order

_Status: current startup direction for the generalized project graph._

The detailed propagation procedures are in
[event_flows/startup_and_project_replay.md](./event_flows/startup_and_project_replay.md).

## Core distinction

Startup must keep three states separate:

- **desired project state** — durable node-instance and project-connection
  declarations owned by `ProjectGraph`;
- **configured project graph** — the root `ConfiguredGraph` that can currently
  be built from available definitions;
- **active execution** — the executable generation currently owned by
  `GraphExecutor`.

Project load reconstructs desired state. It does not require every IV package to
be compiled first, and it does not require audio execution to be active.

## Construction order

1. Construct all long-lived app modules.
2. Bind every bridge/event connection before starting sources that can emit
   project-domain activity.
3. Start only inert/local infrastructure needed by replay and package discovery.
4. Load and parse project persistence synchronously into normalized batches.
5. Replay the project-owned graph batch into `ProjectGraph`.
6. `ProjectGraph` performs one root-build transaction with the definitions
   snapshot currently available, which may be empty/incomplete.
7. Finish server-ready initialization and enable autosave only after replay is
   complete.
8. Start or continue asynchronous package discovery/build/reload work.
9. A completed initial package-provider batch starts a **new** propagation:
   `PackageReload -> NodeDefinitions -> ProjectGraph`.
10. `ProjectGraph` rebuilds the same desired state against the new immutable
    definitions snapshot and submits a new candidate generation to
    `GraphExecutor`.

## Valid initialized state before package realization

After project replay and before package build completion, all of these are
valid:

- requested node instances exist in `ProjectGraph` but some definitions are
  unavailable;
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

## Package compilation is a separate cause

Declaration/watch events may schedule package compilation, but expensive build
work must not keep the initiating application propagation alive.

When asynchronous compilation finishes, completion starts a new cause and uses
the normal package-reload tree. This prevents project replay or some other
source from reaching `ProjectGraph`, scheduling work, and then re-entering
`ProjectGraph` through the completion path before the first cause has unwound.

## One snapshot per instance batch

When `ProjectGraph` rebuilds because definitions changed, it passes exactly one
immutable `NodeDefinitionsSnapshot` to the single `NodeInstances` invocation for
that transaction.

Every top-level requested node and every nested `g.node(...)` lookup/configure
operation uses that same snapshot. A concurrently arriving definition snapshot
can only become input to a later root-build transaction.

## Graph execution activation

`GraphExecutor` may compile/optimize a submitted root graph asynchronously.
Compilation completion does not mutate an active audio pass.

The active generation remains immutable for the duration of a complete pass.
A completed successor can replace it only at a safe boundary after the current
pass finishes. Newer graph revisions may supersede older pending compilation
results.

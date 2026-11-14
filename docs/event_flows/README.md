# Fundamental Project-Graph Event Flows

These files document the current execution-related event propagation procedures.
Each procedure must satisfy the strict tree constraint from
[event_propagation_tree_constraint.md](../event_propagation_tree_constraint.md):
for one source invocation, no application module may be entered more than once.

The procedures are:

- [User node mutation](./user_node_mutation.md)
- [Package refresh](./package_refresh.md)
- [User connection mutation](./user_connection_mutation.md)
- [Startup and project replay](./startup_and_project_replay.md)
- [Node message interaction](./node_message_interaction.md)
- [Graph scope read](./graph_scope_read.md)
- [Manual-control gesture](./manual_control_gesture.md)

Each tree is rooted at one event source entry, opened with
`IV_EVENT_SOURCE(SourceModule, name) { ... }`; linker events cannot be raised
outside one. The current entries are listed in
[Every raise has a source](../event_propagation_tree_constraint.md#every-raise-has-a-source).

The diagrams describe **control flow**. Data may travel in either direction
along an event/request edge. In particular, `ProjectGraph` may pass a mutable
root builder to `NodeInstances`/`GraphConnections`, receive embedding maps or
diagnostics back, synchronously exchange a completed graph for a `CompiledGraph`
with `GraphJit`, and finally stage that result once in `GraphExecutor` without creating
reverse control-flow edges. `GraphExecutor` internally prepares background execution
before realtime activation can become eligible. Realtime/background queue publication,
cutover and persisted-state completion are internal runtime handoffs rather than
app-module event propagation; see
[../realtime_background_execution_and_queues.md](../realtime_background_execution_and_queues.md).

Future sources such as undo/redo, presentation-driven structural changes,
project save collection, and UI-only presentation settings should be designed
later using the same rule. They should not be grafted onto these trees if doing
so creates a diamond or causes one module to process the same cause twice.

The accepted [Project Undo, Redo, And Backup Direction](../project_undo_redo_and_backups_direction.md)
requires a whole reversible user-action batch to use **one** normal coordinated
project-mutation tree. A multi-record undo is not multiple `SocketRpcServer` or
`ProjectGraph` requests, and history does not create a second graph state owner.
Its exact event flow will be documented once the first Vue editing/undo feature
establishes the concrete typed request; do not assume one event per reversed
record.

## Scope of the diagrams

The diagrams emphasize the graph/execution state path. Other state-only children
may be attached to the same tree when they do not create convergence. For
example, package refresh can update `PackageDefinitions`, and `ProjectGraph` can
update `IvModuleSourceIntrospection` once using the combined module-instance/source state it already holds.

Any UI notification that would cause a module such as `SocketRpcServer` to be
re-entered during its own incoming-request cause must be emitted only after that
cause unwinds, as a new source invocation.

## Presentation notifications are batched at the cause's root

No module notifies `SocketRpcServer` partway through a cause. A module's
read-model changes return on the request record it already answers, up to the
cause's root, which publishes them once (implemented 2026-11-12):

- `NodeInstances` returns its instance-list and declaration changes in
  `NodeInstancesProjectGraphRequest`; `ProjectGraph` updates
  `IvModuleSourceIntrospection` once per root build and returns the instance
  list and virtual-node replacements as a `ProjectGraphPublication`.
- In an RPC cause, the publication returns on the request's result builder and
  `SocketRpcServer` publishes it after the response, at the response's revision.
- In a package refresh, it returns through `NodeDefinitionsSnapshotChanged`,
  `PackageDefinitionsPublicationRequest`, and `PackageRefreshTransaction` to
  `PackageWatcher`, which sends one `PackageRefreshPublication` (build outcome,
  catalog change, project changes) as its last edge. Only `rebuildStarted`,
  sent before the build, is a separate cause.
- Startup replay publishes nothing; clients read the replayed state when they
  connect.
- One manual-control cause sends `GraphExecutor` one
  `ProjectGraphGraphExecutorControlsRequest`: the target change, then any
  ready same-layout code to stage.

The tree rule caught this when a refresh reached `SocketRpcServer` through both
`NodeInstances` and `IvModuleSourceIntrospection`. The previous trees are
preserved in
[event_flows_before_root_batched_publication_2026-11-12.md](../historical/event_flows_before_root_batched_publication_2026-11-12.md).

Package-specific ownership and the reason `PackageWatcher` coordinates every package
build cause are specified in
[../package_pipeline_architecture.md](../package_pipeline_architecture.md).

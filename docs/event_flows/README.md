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

The diagrams describe **control flow**. Data may travel in either direction
along an event/request edge. In particular, `ProjectGraph` may pass a mutable
root builder to `NodeInstances`/`GraphConnections`, receive embedding maps or
diagnostics back, synchronously exchange a completed graph for a `CompiledGraph`
with `GraphJit`, and finally offer that result to `GraphExecutor` without creating
reverse control-flow edges.

Future sources such as undo/redo, presentation-driven structural changes,
project save collection, and UI-only presentation settings should be designed
later using the same rule. They should not be grafted onto these trees if doing
so creates a diamond or causes one module to process the same cause twice.

## Scope of the diagrams

The diagrams emphasize the graph/execution state path. Other state-only children
may be attached to the same tree when they do not create convergence. For
example, package refresh can update `PackageDefinitions`, and `ProjectGraph` can
update `IvModuleSourceIntrospection` once using the combined module-instance/source state it already holds.

Any UI notification that would cause a module such as `SocketRpcServer` to be
re-entered during its own incoming-request cause must be emitted only after that
cause unwinds, as a new source invocation.

Package-specific ownership and the reason `PackageWatcher` coordinates every package
build cause are specified in
[../package_pipeline_architecture.md](../package_pipeline_architecture.md).

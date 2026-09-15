# Fundamental Project-Graph Event Flows

These files document the current execution-related event propagation procedures.
Each procedure must satisfy the strict tree constraint from
[event_propagation_tree_constraint.md](../event_propagation_tree_constraint.md):
for one source invocation, no application module may be entered more than once.

The procedures are:

- [User node mutation](./user_node_mutation.md)
- [Package reload](./package_reload.md)
- [User connection mutation](./user_connection_mutation.md)
- [Startup and project replay](./startup_and_project_replay.md)

The diagrams describe **control flow**. Data may travel in either direction
along an event/request edge. In particular, `ProjectGraph` may pass a mutable
root builder to `NodeInstances`/`GraphConnections` and receive embedding maps or
diagnostics back without creating reverse control-flow edges.

Future sources such as undo/redo, presentation-driven structural changes,
project save collection, and UI-only presentation settings should be designed
later using the same rule. They should not be grafted onto these trees if doing
so creates a diamond or causes one module to process the same cause twice.

## Scope of the diagrams

The diagrams emphasize the graph/execution state path. Other state-only children
may be attached to the same tree when they do not create convergence. For
example, package reload can update `PackageDefinitions`, and `ProjectGraph` can
update `NodeSourceIntrospection` once using the combined state it already holds.

Any UI notification that would cause a module such as `SocketRpcServer` to be
re-entered during its own incoming-request cause must be emitted only after that
cause unwinds, as a new source invocation.

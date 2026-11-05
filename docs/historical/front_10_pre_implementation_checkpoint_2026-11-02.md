# Front 10 pre-implementation checkpoint (2026-11-02)

This file preserves status text superseded by the Front 10 implementation. The
current design is in
[Project File Serialization Direction](../project_file_serialization_direction.md),
[Project Graph Application Architecture](../project_graph_application_architecture.md),
and [User Connection Mutation](../event_flows/user_connection_mutation.md).

Before Front 10, the structured `ProjectConnection` persistence and JSON-RPC
adapters were follow-up work. `ProjectPersistence` replayed legacy
`ivModuleInstances.create` and `ivModuleInstances.update` commands one at a time,
which caused one root rebuild for each node command. `NodeInstances` contributed
the old IV-module compatibility read model to persistence, and
`GraphConnections` did not contribute desired connection state.

The old save-policy text also described `ProjectGraph` as the direct persistence
contributor for node and connection declarations. The implemented collection
protocol instead asks the two canonical owners directly: `NodeInstances`
contributes `ProjectNodeInstanceDeclaration` records and `GraphConnections`
contributes `ProjectConnection` records. `ProjectGraph` remains the single
coordinator for replay and live mutation root rebuilds; it does not duplicate
either desired-state set.

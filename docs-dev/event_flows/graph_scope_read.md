# Event Flow: Graph Scope Read

This procedure covers one `graph.getScope` client message: the graph editor's
read of the project root scope. The editor is specified in
[The graph editor is opened on one node](../node_presentation_and_manual_controls_direction.md#the-graph-editor-is-opened-on-one-node).

## Propagation tree

```mermaid
flowchart TD
    SRC["JSON-RPC client message"]
    RPC["SocketRpcServer"]
    PG["ProjectGraph"]
    NI["NodeInstances"]
    GC["GraphConnections"]
    PD["PackageDefinitions"]

    SRC --> RPC
    RPC -->|"graph scope read ⇄ ProjectGraphScope"| PG
    PG -->|"1. desired instances read ⇄ instances"| NI
    PG -->|"2. desired connections read ⇄ connections"| GC
    PG -->|"3. package catalog read ⇄ packages and publication_pending"| PD
```

`ProjectGraph` holds its lock across these reads. Every project mutation holds
the same lock while it changes `NodeInstances` and `GraphConnections`, so one
read describes one revision.

`PackageDefinitions` takes its own lock to copy package build results together
with `publication_pending`. It releases its lock before its synchronous
definitions/project publication, so the scope read does not invert that lock
order. Until publication completes, an unavailable definition stays pending
even if the catalog already contains its successful build result.

`ProjectGraph` returns a classified `loadState`: `waitingForDefinition`,
`waitingForGraph`, `ready`, or `failed`. The UI colors only `failed` nodes red.
Configuration diagnostics and a previously active realization remain separate
facts in the returned node record.

`ProjectGraph` adds to the desired state what only it holds: the current
generation. Each instance placed in that generation gets the ports of its root
bundle, read through the same port tables `GraphConnections` resolves a
`ProjectPortMatcher` against, so a port's index is the one a connection uses.
Each desired connection is marked applied when the generation applied it, so a
connection whose endpoint is missing stays visible.

## Change notification

Every root build sets `graph_changed` on its `ProjectGraphPublication`, which
returns to the cause's root with the build's other read-model changes (see
[README](./README.md)). `SocketRpcServer` publishes it as one `graph.changed`
notification, and each editor re-reads its scope.

The editor also re-reads on `ivPackages.updated`. A failed package build changes
load state even if no new project generation or `graph.changed` is produced.

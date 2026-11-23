# Anonymous port state before frozen invocation reuse

Preserved from [Sequential Port Storage Planning](../sequential_port_storage_planning.md#sample-and-event-streams-share-one-lifetime-and-ownership-model)
at revision `464494d4` on 2026-11-23. The replacement rule recognizes the
local bundle correspondence when a project instance reuses its frozen
invocation across root rebuilds.

> Newly settled (2026-11-09, human co-author): observable state survives an edit exactly
> when its owner is identified in both graphs. A concrete node under no symbolic node has
> no identity, so none of its state survives, including compiler-owned storage that
> holds its outputs; it behaves like a node the edit adds.

> Newly settled on 2026-11-21. Observation changes can require new storage for
> the same immutable configured graph. Within that snapshot, the original
> bundle coordinate identifies an anonymous concrete node. The compiler
> matches its State and port windows using this identity. Compiler-owned
> stages use the configured port they serve and their execution role. A
> bundle coordinate cannot preserve state across a change to the configured
> graph. If an observation cutover cannot preserve the required state, the
> executor keeps the active graph.

# Front 9 status before implementation, 2026-11-02

_Status: historical, non-normative. This file preserves the implementation-status
and open-detail text that was replaced when Front 9 reached its Front 12 dependency
boundary. The current design and status are in
[`configuration_expression_language_direction.md`](../configuration_expression_language_direction.md)
and [`implementation_fronts_direction.md`](../implementation_fronts_direction.md)._

The replaced document status read:

> _Status: normative Front 9 design. The parser, type metadata, evaluator, reference
> resolver, and typed construction path are not implemented yet._

The replaced open-detail list read:

> - The canonical lexical format of a durable project node ID.
> - The ABI for configuration type metadata and typed construction.
> - The metadata representation for named tiled children.
> - The exact application of selectors to set-valued intermediate results.
> - The safe conversion of an authored-node reference into a construction argument
>   across builder scopes.
> - Project-dependent dependency tracking and cache invalidation.
> - The durable representation of future project-owned editable subgraphs.

The first four items are now concrete implementation decisions. The live `NodeRef`
conversion and its project-revision dependencies remain owned by Front 12. Editable
project subgraphs remain future `ProjectGraph` work and are not part of Front 9.

Two normative documents also described the earlier `GraphConnections` implementation
checkpoint. Their replaced text said that the resolver searched descendant virtual
nodes, used numeric tiled-child selectors, and accepted `ProjectSubgraphSelector`, and
that Front 9 still had to replace this behavior. The shared scope-local resolver and
named tiled-child metadata now replace that checkpoint.

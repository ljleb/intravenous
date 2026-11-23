# Front 13 Predecessor Argument Introspection False Start

_Status: non-normative record of a Front 13 design that was superseded on
2026-11-03. The current design is in
[Node Interaction And Presentation Communication Direction](../node_interaction_and_presentation_communication_direction.md)
and [Implementation Fronts Direction](../implementation_fronts_direction.md)._

The earlier design required `NodeMessageContext` to expose the predecessor
node's complete construction arguments. A replacement handler could then copy
that argument list and change selected values or structured subvalues.

The earlier Front 13 scope included this requirement:

> exposing the predecessor node's current configuration arguments to a
> replacement handler and allowing selected arguments/structured subvalues to
> be replaced while preserving the rest

The node-interaction design also stated:

> Captured values remain useful for ordinary handler-specific policy, but they
> should not be the only way to remember a node's prior construction arguments.
> Replacement needs a first-class view of the predecessor's typed/structured
> configuration values so a handler can express "change these arguments,
> preserve the rest".

The design discussion found that this mechanism duplicates data that authored
handlers already own. A leaf-node handler can read the node's construction
state through `this`. A module-node handler can capture the values that it must
preserve. The handler can therefore supply the complete typed argument list for
the successor.

The accepted replacement API does not recover predecessor arguments from
`ConfiguredNodeInstance::configuration_lifetime`. It accepts a successor
definition and the complete typed arguments for that successor. Package code
serializes those supplied arguments before control returns to the application.
`ProjectNodeReplacement` stores the complete durable successor request.

This decision removes the proposed public dynamic-field API, member-pointer
replacement API, and `NodeMessageContext::arguments<...>()` API. It does not
remove Front 9 support for structured values. A handler can pass a structured
value when a registered definition uses one.

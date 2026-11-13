# Vue Port Lists Before First-Item Alignment

_Status: historical design and implementation checkpoint, preserved on 2026-11-13._

Sources: `docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`, immediately before the owner
requested that grouped port lists put their first port under the grouped icon
and remove the connection summaries below the port rows. The preceding source
used a grouped-icon cap above or below the ordinary port list; only connection
choosers aligned their first entry with the control that opened them.

Replacements: [Port Interactions](../node_presentation_and_manual_controls_direction.md)
and [Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous grouped-icon contract

means every port in the group has at least one connection. The ordinary expanded port panel
uses the same icon. These states count desired connections; dashed outlines
continue to identify pending connections.

## Previous source checkpoint

Applied connections fill their port circles. Unapplied desired connections
use dashed circles. Port tooltips identify their connections and application
state. Expanded slots also list connections. Deletion is an extension command.
Type conversion remains a backend decision.

Grouped input and output icons now show empty, partly connected, and fully
connected states. The right circle fills when any port has a desired connection;
both fill when every port has one. The ordinary expanded port panel cap uses the same icon.
Pending desired connections retain dashed outlines.

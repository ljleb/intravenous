# Vue Connection Display Before Lane Selection

_Status: historical implementation checkpoint, preserved on 2026-11-13._

Source: `docs/vue_client_implementation_stages.md`. The owner requested lane
selection with incoming and outgoing wires, plus wires that separate when a
grouped port slot opens. The replacement is in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous status

Applied connections fill their port circles. Unapplied desired connections
use dashed circles. Port tooltips identify their connections and application
state. Expanded slots also list connections. Only the active gesture draws
a wire; permanent wire layout is not implemented. Deletion is an extension
command. Type conversion remains a backend decision.

The previous UI verification step also stated: "No permanent wire layout is
implemented yet." That restriction described the checkpoint, not a prohibition
on the selected-lane connection display requested afterward.

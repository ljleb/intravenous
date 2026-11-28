# Connection release before incompatible drops disconnected an end

Preserved on 2026-11-24 from
[`node_presentation_and_manual_controls_direction.md`](../node_presentation_and_manual_controls_direction.md#disconnection-and-endpoint-movement).
The same section describes the replacement rule.

> The graph stays unchanged during the drag. Release in empty editor space
> removes the chosen end. Release on a compatible port of the same direction
> moves it there. Release on the original port does nothing. Release on an
> incompatible port, a grouped slot without a chosen individual port, another
> control, or outside the editor cancels. Escape, lost pointer capture,
> pointer cancellation, or window blur also cancels.

The previous [implementation checkpoint](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint)
described the same behavior.

> The graph changes only on release. Empty editor space removes the chosen end;
> a compatible port replaces it. Invalid drops and cancellation leave the graph
> unchanged. The preview stays anchored at the opposite end, including grouped
> port slots and multiple opposite endpoints.

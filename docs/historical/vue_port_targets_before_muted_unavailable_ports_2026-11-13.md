# Vue Port Targets Before Muted Unavailable Ports

_Status: historical design and source checkpoint, preserved on 2026-11-13._

Sources: `docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`. The owner retained candidate-target
feedback but requested muted colours for unavailable ports instead of rings.
The owner also requested that passing over outputs must not open input
popovers during a new output-to-input drag.

Replacements: [Port Interactions](../node_presentation_and_manual_controls_direction.md)
and [Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous hover contract

During a connection drag, hovering another node's port area exposes the
appropriate destination side. Grouping changes presentation only. Each port
retains its identity, and there is no limit on the underlying port count.

## Previous source checkpoints

During a
connection drag, grouped destinations expand the opposite port direction.
Direct handles stay exposed instead of acquiring an expansion over the drop
target.

The target ring in `NodePorts` is the `.target .port-circle` box shadow using
`--vscode-focusBorder`. For a new connection, `acceptsPortConnection` checks
opposite directions and equal sample/event kinds; it does not establish that
a converter exists for the two port types. For moving an existing end,
`canMoveConnectionEnd` applies its additional record/type rules. Filled and
dashed circle states describe existing connections separately from this ring.

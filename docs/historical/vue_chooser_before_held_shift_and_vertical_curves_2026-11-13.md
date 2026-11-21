# Vue Chooser Before Held Shift And Vertical Curves

_Status: historical implementation checkpoint, preserved on 2026-11-13._

Source: `docs/vue_client_implementation_stages.md`, immediately before the owner
requested that connection choosers require held Shift and wire endpoint tangents
use a vertical direction. The replacement is in
[Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).
The accepted interaction is in
[Disconnection And Endpoint Movement](../node_presentation_and_manual_controls_direction.md#disconnection-and-endpoint-movement).

The preceding chooser implementation opened on Shift-hover but had no keyup
handler. A tap could leave the chooser open until the pointer left, Escape
was pressed, or the window lost focus. It checked Shift on pointer entry and
keyboard keydown, but did not recover the modifier state on pointer movement
within an already hovered port.

## Previous wire checkpoint

`SelectedLaneWire.selfLoopY` distinguishes connections within the selected node
from connections to other nodes, even if their port rows have identical screen
coordinates. Self-connections leave the output to the right, return below the
ports on separate horizontal paths, and enter the input from the left.
Connections between different nodes keep their previous curves. These changes
affect drawing only; they do not change connection records or lane dimensions.

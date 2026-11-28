# Port connection chooser before right-click

The following excerpts from `node_presentation_and_manual_controls_direction.md`
at revision `9f90e9bb` describe the Shift-hover chooser and the separate
right-click delay controls. The owner replaced these entry points with a single
right-click chooser. See the
[current connection gestures](../node_presentation_and_manual_controls_direction.md#disconnection-and-endpoint-movement).

> The port connection chooser shows a **Feedback delay (samples)** knob on each
> detached connection. Hovering a delay knob lets the user release Shift while
> the chooser stays open. Right-clicking either endpoint opens those controls.

> A normal primary drag starts a new connection, including when the port already
> has connections. Shift-hover over any connected port opens a chooser, including
> when the port has exactly one connection. Pressing Shift while already hovering
> the port also opens it. The first connection circle occupies the hovered port
> circle's position; it replaces the chooser's decorative icon. Its entry can
> therefore be picked up without moving down the list. Other entries remain
> available for explicit selection. Near the bottom of the viewport, the remaining
> entries grow upward with the first connection still at the hovered port.

> Drag a chooser entry with the primary button to move that connection end.
> Direct Shift-drag can still pick up a sole connection. Shift-click remains a
> fallback, not a required opening click. Opening the chooser does not select a
> connection or change the graph. Right-click receives no new menu.
> The connection chooser stays open only while Shift is held. Releasing Shift
> hides it. An ordinary grouped-port list remains governed by its normal hover
> rules. Once a chooser entry has been picked up, releasing Shift does not
> cancel that drag.

The former folded-port gesture came from
[`vue_presentation_refinement_direction.md`](../vue_presentation_refinement_direction.md#folded-inputs-extend-the-existing-lane-header)
at the same revision:

> Hover over the input or output controls to open that column's popover.
> The gap between the columns closes the popover when the pointer enters it.
> The popover stays open while the pointer moves into it or while a knob is in use.
> Hidden rows use the same knob size, height, and spacing as the visible rows.
> The popover can widen for port names and start below visible rows to keep their
> controls clear. Each revealed port uses its own socket for cable endpoints.
> Scroll the additional rows when they exceed the available viewport height.
> During a cable drag, hover over compatible controls to reveal their column.
> Shift retains the separate per-port connection chooser.

The [implementation checkpoint](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint)
records the hover state, alignment, and pointer-boundary handling for the
earlier interaction.

## Interim right-click port list

The November 25 working revision of
[`vue_presentation_refinement_direction.md`](../vue_presentation_refinement_direction.md#folded-inputs-extend-the-existing-lane-header)
briefly used a click to open both levels. The owner returned the port list to
hover and retained right-click for the connection chooser.

> Right-click the folded socket to open that column's port popover. Right-click
> a port inside it to open that port's connections. **Back to ports** or Escape
> returns to the list. Click outside to dismiss the popover.

## Interim feedback-delay color

The November 25 working revision of
[Node Presentation and Manual Controls](../node_presentation_and_manual_controls_direction.md#lane-selection-and-connection-display)
used an OKLCH hue midpoint for the feedback-delay knob. The owner chose a
fully saturated green after seeing the midpoint in the graph.

> The delay knob uses the midpoint along the green hue path between the input
> and output colors.

The next working revision used saturated green on the knob and its preview.
The socket kept its port color, and the cable kept the endpoint gradient even
when dashed. A short-lived proposal extended green to the socket and wire.
The owner settled on a gradient on the knob face and the regular port and
cable colors. See the [current direction](../node_presentation_and_manual_controls_direction.md#lane-selection-and-connection-display).

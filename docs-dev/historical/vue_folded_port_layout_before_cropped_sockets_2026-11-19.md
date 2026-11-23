# Folded port layout before cropped sockets

Historical record, 2026-11-19. The owner chose cropped sockets at the
header edge and one continuation for both columns. This preserves the
earlier geometry and hover contract. See the
[replacement](../vue_presentation_refinement_direction.md#folded-inputs-extend-the-existing-lane-header).

## From vue_presentation_refinement_direction.md

### Folded inputs extend the existing lane header

When the available lane-header height requires folding inputs, the **last
visible input remains an ordinary input**: it retains its actual knob, port
circle, name, connection endpoint, and normal interactions. Do not replace
that input's knob with a generic stacked-port icon. Hovering or focusing this
last input also opens the continuation containing the remaining inputs.
It is both a real control and the expansion anchor; interacting with its knob
must not accidentally start a connection or discard a live manual gesture.

Treat the continuation as the **missing portion of the same lane header**, not
a freely repositioned dialog. Anchor its first row to the last visible input
row; use exactly the same knob and port columns, row heights, spacing, and
input-name alignment as the direct rows. The anchor knob, every already-visible
knob, and every connection endpoint remain at **identical screen coordinates**
while opening, navigating, or closing the continuation. The anchor is displayed
once, not duplicated as two independently interactive controls. Hidden inputs
appear below the anchor in their definition order. If the viewport cannot show
the whole continuation, scroll/clip the additional rows or otherwise handle
the available space **without moving the anchor**, flipping the panel to an
unrelated location, or changing the lane's permanent height. Keep the extended
header visually contiguous with the ordinary header and above the lane content;
avoid covering input labels with cables.

The same positional-continuity principle applies to grouped output ports and
their eventual gain controls where appropriate. It does not replace the
separate connection-chooser behavior, hover lifetime, drag-source closing,
or per-port connection identity. Connection endpoints must follow the visible
port's actual geometry while the extension is open and collapse coherently
when it closes. The earlier standalone folded-slot placement is preserved
[historically](./vue_nested_port_clip_scale_and_ruler_input_before_2026-11-16.md#previous-folded-port-slot-geometry).

When a side has hidden ports, show one dim ellipsis at the bottom of the
header. Center the three spaced dots below the last visible knob or port.
The [earlier indicator](./vue_port_overflow_indicator_before_header_footer.md)
used a vertical stack.
As the lane shrinks, step from full-size knobs to compact knobs and then to
the folded port indicators. At mini and smaller heights, show a single port
symbol or a stack on each side. Each symbol opens a popover with full-size
controls and enough room for names to wrap.

## From node_presentation_and_manual_controls_direction.md

_Accepted clarification, 2026-11-14:_ use the available height of the lane's
left area to show ports directly. Inputs and outputs keep separate columns.
Each column retains reflected definition order. If every port fits, show all
of them. Otherwise, reserve one handle for a popover that contains the remaining
ports. Compute each direction independently. Hidden-port connection indicators
summarize only that remainder. Existing connections and drag previews attach to
the direct handle when visible, or to the folded handle when hidden.
Fitting must account for the complete row height, including its knob. Knobs
and wire handles keep separate drag targets. In narrow lane labels, keep those
targets reachable even when names or numeric readouts need to be hidden.

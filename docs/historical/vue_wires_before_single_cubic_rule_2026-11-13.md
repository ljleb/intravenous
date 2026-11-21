# Vue Wires Before The Single Cubic Rule

_Status: historical design and source checkpoint, preserved on 2026-11-13._

Sources: `docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`. The owner requested one simple
Bezier rule, removed self-connection detours, and clarified that cursor colour
must depend on position. The owner explicitly retained the gradient from the
fixed port's colour to the cursor colour.

Replacements: [Connection Display](../node_presentation_and_manual_controls_direction.md#lane-selection-and-connection-display)
and [Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous curve contract

A connection between an output and an input of the same node must form a loop
away from the port row. It must not double back through its output port.
Separate visible self-connections use separate return paths. Grouped endpoints
still share one drawn wire until the slot opens.
By default, the output's Bezier control point lies to its left and the
input's control point lies to its right. Self-connections and equal-height
endpoints retain an arc away from the port row instead of a retraced curve.


## Previous preview contract

The preview uses a Bezier curve with the same routing style as a finished
connection. Its fixed end keeps that port's color: yellow for an input and blue
for an output. The cursor end is yellow over an input group or input popover,
blue over an output group or output popover, and a continuous blend between
the input and output columns. Away from a port group, use the closest port
row's measured column positions. The fixed-end tangent retains its port's
direction; the free-end tangent changes with the blend toward input-right or
output-left. These rules also apply when moving an existing connection end.
Preview color is a positional cue; it does not change connection compatibility
or commit a connection before release.


## Previous source checkpoints

`SelectedLaneWire.selfLoopY` distinguishes self-connections from connections
to other nodes, even if their port rows have identical screen coordinates.
Output tangents point left and input control points sit to the right. Wires
between different rows use one cubic curve. Self-connections and equal-height
endpoints use two cubic segments through a lower return arc, so their
horizontal tangents do not collapse into a retraced line.
These changes affect drawing only; they do not change connection records or
lane dimensions. Previous routing is retained in
[Vue Ports Before Single-Connection Choosers](./historical/vue_ports_before_single_connection_choosers_2026-11-13.md)
and [Vue Chooser Before Held Shift And Vertical Curves](./historical/vue_chooser_before_held_shift_and_vertical_curves_2026-11-13.md).


`connectionCursorOutputFraction` uses the closest measured port row to blend
between input and output column centers. A hovered port group or popover
overrides that blend with its exact input/output color. Geometry changes
refresh `previewColorColumns`; pointer movement interpolates those cached
coordinates without another full layout measurement. `previewPortGroup` tracks
the actual hit for the color cue independently of the source group's hover
suppression during a drag. Fixed tangents preserve their port direction and
the cursor tangent interpolates between the two directions. Light tests cover
color bounds, intermediate values, popover overrides, different row layouts,
and curve tangents in both drag directions. Build, test execution, and manual
color checks remain for the owner.

## Previous manual colour checks

18. Start a new connection at an input. Its fixed end must be yellow and the
    preview must curve. Move over the input controls, across the gap, and over
    the output controls: the cursor end must change from yellow through a
    blend to blue. Repeat from an output, whose fixed end must stay blue, and
    from a connection chooser. Expanded input/output popovers must give their
    exact side color. Check that theme changes also update preview colors.


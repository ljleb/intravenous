# Vue Ports Before Single-Connection Choosers

_Status: historical design and source checkpoint, preserved on 2026-11-13._

Sources: `docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`, immediately before the owner
requested grouping above one port per side, choosers for single connections,
and a first entry under the pointer. Source previously displayed up to two
ports directly and placed a grouped icon above the chooser entries.

Replacements: [Port Interactions](../node_presentation_and_manual_controls_direction.md),
[Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous grouping proposal

The owner proposed the following interaction on 2026-11-12. It is a direction
for the connection-editing stage, not a claim that these gestures are implemented
or that all details are settled:

- The lane's left area can expose individual port icons while they fit.
- When the ports do not fit, group their visible controls. Grouping changes
  presentation only; each underlying port retains its own identity.
- Hover can expand a temporary panel over the lane's display area, exposing all
  ports without permanently increasing the lane's size.
- A user can select an individual port and drag a connection from it.
- While dragging from an output, hovering a destination port area expands its
  inputs. While dragging from an input, it expands the outputs.

Any earlier connection-editing subset must preserve arbitrary port counts,
stable endpoint identity, and space for this expansion. Do not commit to a
fixed number of connection handles, merge ports semantically, or require a
permanently taller lane. Opening/closing timing and the rest of the input
interactions remain to be decided with the owner.


## Previous endpoint tangents

Wire endpoint tangents are vertical, including self-connections, so a cable
leaves and approaches its port across the row rather than along neighbouring
port icons.

## Previous chooser contract

A normal primary drag starts a new connection, including when the port already
has connections. Shift-drag moves an existing connection end. With exactly one
connection at that port, it picks up that end immediately. With several
connections, Shift-hover opens a chooser without a click. Pressing Shift while
already hovering the port also opens it. Drag a chooser entry with the primary
button to pick up that connection end. Shift-click remains a fallback, not a
required first click. The editor must not pick an arbitrary connection.
Right-click receives no new menu.

## Previous implementation checkpoint

`SelectedLaneWire.selfLoopY` distinguishes self-connections from connections
to other nodes, even if their port rows have identical screen coordinates.
Wire endpoint tangents are vertical so a cable does not start along the row
of adjacent port icons. For different rows, the curve follows their vertical
order. Equal-height endpoints use a downward arch. Self-connections use
separate downward arches, with control points below their ports.
These changes affect drawing only; they do not change connection records or
lane dimensions. The preceding horizontal routing is retained in
[Vue Chooser Before Held Shift And Vertical Curves](./historical/vue_chooser_before_held_shift_and_vertical_curves_2026-11-13.md).

With one connection, Shift-drag picks up that end; with several, Shift-hover
opens a chooser with draggable entries. Pressing Shift while already over a
port works too. Shift-click remains a fallback. Right-click receives no new menu.

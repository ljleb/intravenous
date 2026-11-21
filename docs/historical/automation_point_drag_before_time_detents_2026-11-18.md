# Automation Point Drag Before Time Detents

_Historical snapshot. Preserved 2026-11-18 after the owner chose plain placement,
Shift to keep the point type, and a two-sided time detent._

The revised rules are in the [Vue presentation direction](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping),
the [cyclic automation direction](../automation_and_suspendable_coverage_direction.md),
and the [builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).

## Previous Vue presentation direction

Keep the current cyclic `CurvePointEvent` semantics and Tock/computed
interpreter: point IDs, outgoing `empty`/`hold`/automatic `bezier` segments,
tension, stable equal-index order, and sparse coverage do not change.
Refine the Vue curve editor's gestures as follows:

- **Left-click and drag a point** to move it. Moves preserve point order,
  segment types, and tension. Grabbing an unselected point leaves the current
  selection alone. Grabbing a selected point moves the selected points together.
- **Hold Shift during a single-point drag** to place the point in the
  surrounding curve, allowing it to pass other points. Pressing or releasing
  Shift keeps the current position and segment styles until further movement.
- **Left-drag empty space** to select the points inside the rectangle. Ctrl
  toggles those points and Shift adds them.
- **Shift+click a point** deletes it.
- **Right-click an existing point** opens the segment menu for its outgoing
  curve type, tension, and **Delete point**.
- **Right-press empty space** adds a point at the nearest sample and starts
  placing it immediately. Point hit testing takes precedence. The first point
  on a blank curve is empty. A new point stays in placement mode until release.
  The [previous gestures](vue_lane_and_point_selection_before_2026-11-18.md)
  remain available for context.
- Provide a **tension handle near each applicable control point**, manipulable
  without entering a separate form. Display and edit the same `[0,1]` tension
  parameter used by the backend, keeping exact linear interpolation at `0` and
  the automatic clamped curve at `1`. Do not introduce manual Bézier handles.

Placement in a covered segment inherits its type and tension. Placement in an
empty gap connects to the nearer edge by sample distance along the index
circle. Use the neighboring covered segment's style, or Bézier at tension 1
when the edge has no covered neighbor. At equal distances, connect toward
the following edge. The other half stays empty. As placement changes the
connection, restore any temporary segment edit that no longer applies.

Point edits continue through typed node messages, draft reconciliation and one
undoable gesture per completed manipulation. Insertion publishes on press and
sends periodic updates as the pointer moves. Each placement update applies
the point and related segment edits together. The handle, menu, and drag
interactions must remain coherent across presentation remounts and accepted
backend revisions; no direct mutation of DSP storage from Vue.

[Earlier point movement and insertion](automation_point_drag_before_placement_2026-11-18.md).

## Previous cyclic automation direction

Coincident points preserve an instantaneous jump in their input-event order.
The first point at a sample index supplies the incoming segment's endpoint;
the last point supplies the outgoing value, policy and tension. Intermediate
zero-length segments contain no sample positions. Equal-index ordering must be stable across reconstruction. The point source
uses persistent point IDs to break equal-index ties; generated events retain
that producer order without adding point IDs to `CurvePointEvent`. An
interpreter consumes equal-time events in their stable supplied order, including
after upstream event transformations. At the shared index itself, the last
point's outgoing policy determines the value or absence of coverage.

## Previous builtin automation guide

The Vue presentation creates an empty point list for a new curve. The clip ruler
shows seconds and sample positions. Zoom in to see individual sample marks.

- Right-press empty space to add a point at the nearest sample and start placing it.
- Drag an existing point to move it without changing the order or segment types.
- Hold Shift while dragging one point to place it among its neighbors.
- Shift-click a point to remove it.
- Right-click a point to set its outgoing segment or choose Delete point.
- Drag the diamond beside a Bézier point left or right to adjust its tension.

A point placed in a covered segment inherits its type and tension. A point
placed in a gap connects to the nearer edge and leaves the other side empty.
When there is no neighboring style, the connection uses Bézier tension 1.
The first point on a blank curve is empty. See the
[previous insertion behavior](./automation_point_drag_before_placement_2026-11-18.md).

## Nearest-edge behavior on re-drag

An intermediate design in the 2026-11-18 refinement used this rule for both
insertion and re-drag: “Placement in an empty gap connects to the nearer edge
by sample distance along the index circle.” The owner kept the rule for
insertion and moved the attachment change on re-drag to the crossing of the
other curve's edge. See the [current drag rules](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).

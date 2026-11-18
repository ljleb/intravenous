# Lane and automation point selection before the 2026-11-18 revision

_Historical, non-normative. Preserved on 2026-11-18 after the owner revised
selection, point creation, and Delete routing._

From [Vue presentation refinement, §5](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).

- **Left-click and drag a point** to select/manipulate that existing point.
  A plain left click on a point must not create or delete another point.
- **Shift+click a point deletes it**, consistent with the existing Shift
  gesture for removing port connections. This is the immediate deletion
  gesture; do not retain right-click-to-delete as the primary action.
- **Right-click an existing point** opens a context menu for its outgoing
  curve type (`empty`, `hold`, `bezier`), tension, and **Delete point**.
- **Left-click empty curve space** adds a point at that position. Point
  hit-testing takes precedence over creation. The owner selected this gesture
  on 2026-11-18, replacing
  [right-click creation](./historical/vue_automation_right_click_creation_2026-11-18.md).

From [Node presentation and manual controls, navigation and selection](../node_presentation_and_manual_controls_direction.md#navigation-selection-and-lane-order).

- Support multiple selected lanes. Ctrl-click toggles a lane in the selection.
  Shift-click extends the selection across a range of lanes.
- Delete removes the selected node or nodes without a confirmation dialog.
  Ctrl + Z restores the deletion through project history. As a general policy,
  avoid confirmation dialogs for ordinary reversible project edits.

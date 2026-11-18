# Automation point creation by right click

Historical excerpts preserved on 2026-11-18. The owner chose left-click
creation and added a deletion action to the point menu.
[Current automation gestures](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping)

From `docs/vue_presentation_refinement_direction.md` at revision `7a1e018f`:

> - **Right-click an existing point** opens a context menu for its outgoing
>   curve type (`empty`, `hold`, `bezier`) and relevant segment options.
> - **Right-click empty curve space** adds a point at that position, replacing
>   double-click-to-add. Point hit-testing takes precedence over creation.

From `docs/automation_and_suspendable_coverage_direction.md` at the same revision:

> The backend segment and tension definitions above are unchanged. The Vue editor
> must eventually expose tension via a handle near each point; its interaction
> semantics, right-click outgoing segment menu, right-click empty-space addition,
> left-click manipulation, and **Shift+click point deletion** are defined in
> [Vue Lane And Automation Interaction Direction](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).

From the first sample-editing pass of the builtin guide:

> - Right-click empty space to add a point at the nearest sample.
> - Right-click a point to set its outgoing segment.

[Current builtin guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)

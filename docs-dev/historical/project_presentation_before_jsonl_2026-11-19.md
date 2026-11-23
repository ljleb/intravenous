# Presentation state before JSONL storage

_Historical. Preserved on 2026-11-19 when project saves became the single
source of shared presentation state._

[Lane navigation and order](../node_presentation_and_manual_controls_direction.md#navigation-selection-and-lane-order)
previously said, “Save the displayed order in `.intravenous/graph-layout.json`.”

[Automation value display and panning](../vue_presentation_refinement_direction.md#value-display-and-panning-2026-11-18)
previously said, “The editor saves the display mode, value ruler, pan, and zoom
for each curve in the project's `.intravenous/graph-layout.json`.”

[Vue client implementation stages](../vue_client_implementation_stages.md#editor-managed-ruler-snapping-and-combined-zoom)
previously described saving inactive Beats arguments and Custom routes through
`GraphLayoutStore` before applying the graph edit. The sidecar also stored
global view geometry. Project autosave used a one-second debounce.

The [current save rules](../project_undo_redo_and_backups_direction.md#relationship-to-project-jsonl-saves-and-git)
store shared presentation state through server-owned project saves and keep
global navigation in the current editor session.

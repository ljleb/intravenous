# Port tooltip requirements before status-first presentation, 2026-11-16

_Status: non-normative historical preservation. Source:
`docs/node_presentation_and_manual_controls_direction.md`, “Proposed port area
and temporary expansion” and “Knob gestures and discovery”, from the repository snapshot `intravenous(20261116-145557).zip` after the
preceding `intravenous_vue_ui_refinement_docs_2026-11-16.patch` was applied,
before this incremental patch. Preserved on 2026-11-16 because the owner
superseded ordinary per-port/knob tooltip information with a quieter system
status-bar presentation. Replacement:
[Vue Contextual Information](../vue_presentation_refinement_direction.md#6-contextual-information-status-bar-before-tooltips)._

## Earlier port detail presentation

> Port details remain available in tooltips.

## Earlier knob help presentation

> A compact VS Code status item reports the hovered knob's shortcuts and the
> active drag's value and sensitivity; its tooltip holds fuller help. Clear it
> when the relevant context or focus is lost.

The status-item semantics and accessible descriptions remain in the accepted
contract; indiscriminate per-element hover tooltips do not.

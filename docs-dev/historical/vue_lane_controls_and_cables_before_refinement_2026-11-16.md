# Vue lane controls and cables before the 2026-11-16 refinement

_Status: non-normative historical preservation. Source:
`docs-dev/node_presentation_and_manual_controls_direction.md`, paragraphs in
“Lane selection and connection display” and “Knob gestures and discovery”, as
present in repository snapshot `intravenous(20261116-145557).zip` before this
patch. Preserved on 2026-11-16 after the owner superseded selection-only cable
visibility and permanently visible inline knob values. Current replacement:
[Vue Lane, Meter, Port-Control, Cable, And Automation Interaction Direction](../vue_presentation_refinement_direction.md), §§2 and 4._

## Earlier lane-connection display requirement

> The owner requested this display on 2026-11-13. Selecting a lane displays its
> incoming and outgoing connections. Hovering another lane's grouped port slot
> does not transfer selection to that lane.

## Earlier knob/value layout requirement

> Place the control between its port circle and displayed name. Keep the knob
> and wire handle as separate pointer targets. Show the logical value prominently;
> when finite minimum and maximum bounds exist, also show the normalized 0–1
> value with less emphasis.

These earlier requirements remain useful for explaining the implementation
checkpoint, but no longer govern new Vue work where they conflict with the
accepted 2026-11-16 intent.

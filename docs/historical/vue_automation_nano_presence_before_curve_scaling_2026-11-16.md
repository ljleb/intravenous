# Automation nano presence before continuous curve scaling

Non-normative historical text. Preserved from
`docs/vue_presentation_refinement_direction.md`, sections 5 and 7, before the
owner's 2026-11-16 request to keep the curve shape in nano mode. The replacement
is in [Vue presentation refinement](../vue_presentation_refinement_direction.md).

The previous zoom contract was:

> Honor the lane's existing **`nano`**, **`mini`**, and **`editable`** vertical
> zoom categories. In `nano`, show **where the curve has a value in time** using
> its exact coverage: an outgoing `empty` segment means no coverage until the
> next appropriate point, including the circular wrap and equal-index rules.
> Do not draw interpolated sample values as though they exist in those gaps.
> In `mini`, display the curve shape and empty portions legibly but disable
> point/tension/segment editing. Only `editable` displays interactive points,
> tension handles and the editing controls. Changing zoom level does not change
> point data, bounds or selection identity. Sample-accurate time snapping remains
> required for all edits in `editable`.

The corresponding acceptance wording was:

> Test all four bound configurations for the automation value axis, coverage-only
> `nano` view, non-editable `mini`, and editable points/handles in `editable`.

The earlier source-tuning guide referred to `.curve-coverage` for the nano-only
fill and `.output-label` for the meter's left-hand label and scale button. These
selectors are removed by the same owner refinement: nano keeps the curve path,
and meter scale selection moves to a context menu on the visualization.

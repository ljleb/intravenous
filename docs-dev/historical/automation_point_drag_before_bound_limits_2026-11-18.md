# Point drags before bound limits

Preserved from the working copy of `docs-dev/vue_presentation_refinement_direction.md`
on 2026-11-18. The owner subsequently chose to apply enabled bounds to a point
as soon as it is dragged. Changing the bounds alone continues to preserve
existing stored point values.
[Current point drag rule](../vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories)

> Point edits keep their authored `Sample` values and never rescale the vertical
> axis to fit the points. Use the existing lane-level vertical zoom. Save bound
> settings in the project with the automation module so they survive presentation
> remount and project reload.

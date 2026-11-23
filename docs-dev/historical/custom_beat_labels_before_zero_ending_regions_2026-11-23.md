# Custom bar labels before counting backward in Regions ending at zero

Preserved from [the timeline design](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping)
at revision `7ada2bac` on 2026-11-23. Beats publishes a separate indexed
Region ending at sample zero. The current rule gives that Region the same
bar numbers in Custom and Beats modes.

> The Custom ruler numbers the bars in each indexed depth-zero Region from
> its start. Recover each bar's event index from the run's first index and
> stride. Add one to get the bar number in the first half of the sample circle
> relative to the Region start. In the second half, also subtract the Region's
> total event count so the last bar there is 0. Marks read by time supply lines
> and snap targets.
> Coincident bar marks with conflicting labels share one line without a number.

# Ruler grid before editor-managed snap nodes

Preserved on 2026-11-19 from the project playback and Vue presentation
direction documents. The owner accepted Seconds, Beats, and Custom with
editor-managed nodes. See the [replacement design](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping).

## Project playback and presentation grids

_Accepted design, 2026-11-19._

A Timeline node represents a connection to the project editor. Its `beats`
port accepts BeatEvent Coverage. Place Timeline nodes wherever they belong
in the project graph, including inside subgraphs. Use graph connections to
route beat generators and transforms to their inputs.

The ruler's top-right popover selects Seconds or Beats and saves the choice in
the project graph layout. Seconds is the default. It shows time graduations
and disables grid snapping. Beats shows event marks and snaps to the visible
marks. The ruler and automation lanes draw only the selected time grid. The
editor requests Timeline event windows in Beats mode.

The editor observes the resolved inputs of every Timeline node in the
project. The observation carries event timestamps and payloads, with the
timing and Coverage of the receiving port. The UI owns the merge rules.
For beats, it combines events by sample index and keeps the smallest
`BeatEvent.depth` at each coincident index. Each viewport selects the
displayed marks for its zoom level. The ruler and the snap gesture use the
same list.

The viewport picks marks by a minimum horizontal pixel distance at its current
scale. Closer candidates give way to coarser marks. The Beats preview uses the
same distance rule. A query may include marks beyond the viewport so the
editor can redraw and snap immediately during nearby pans and zoom changes.

The timeline draws the ruler's visible beat marks continuously over each run
of time-aware lanes, including automation lanes. While the viewport changes,
the editor can project overlapping data from the last ready observation window
until the replacement arrives. A point created within the snap radius starts
at the nearest visible mark. During
a drag, use the same pointer-travel detents as value snapping. A beat and a
neighboring point at the same sample share a detent. Highlight the active time
guide in blue. The displayed background playhead at pickup also provides a
detent, in both time-grid modes. Between detents, quantize to the nearest
project sample. A grid change affects subsequent edits. Existing point
positions are retained.
[Earlier radius-only drag snapping](./automation_beat_snap_before_detents_2026-11-19.md).

The [previous ruler input and cover selector](./ruler_coverage_input_before_timeline_nodes_2026-11-19.md)
is preserved with its source-selection rationale.

## Vue presentation refinement

The project time ruler uses the inputs of authored Timeline nodes as described
in [Timeline nodes and project snapping](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping).

## Built-in package README

The `Beats` section of `src/intravenous/builtin_packages/README.md` used to say:

> Connect the Beats output to the `beats` input of `iv.builtin.timeline` to use
> it as a project snap grid. The ruler combines marks from the project's Timeline
> nodes, including Timeline nodes inside modules. Seeking and automation point
> editing snap to the marks visible at the current zoom level.

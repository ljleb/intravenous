# Automation beat snapping before drag detents

This records the previous snapping behavior from the project playback and Vue
presentation directions on 2026-11-19.
[The current snapping policy](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping)
uses a detent during a drag.

When creating or dragging an automation control point, quantize to integer
project samples while the pointer moves. Use a visible beat mark within
the snap radius when available. With no eligible mark, use the nearest
project sample. A change to the grid affects subsequent edits. Existing
point positions are retained.

The editor uses the same visible event marks for drawing and snapping.
Keep the radius in screen pixels across horizontal zoom levels. The beat
source supplies timestamps in project samples. The point editor applies
snapping during creation and drag and saves the chosen sample index.

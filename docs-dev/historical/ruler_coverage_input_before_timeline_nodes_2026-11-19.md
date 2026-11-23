# Ruler Coverage input before Timeline nodes

_Historical record, 2026-11-19. The owner replaced the fixed-header input
and selected cover with project-wide observations of the inputs on authored
Timeline nodes. See the [accepted Timeline design](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping)._

## Project playback and presentation grids

From [Project Playback And Presentation Grids](../project_playback_and_presentation_grids_direction.md).

> The generated-grid section remains a proposed presentation feature.

## Project ruler input for graph-authored snapping (accepted 2026-11-16)

The **fixed-height header of the top project time ruler** exposes an **event
input**. A graph connection to this header configures the time marks used for
presentation snapping, particularly automation control-point editing. Its
header need not be resizable. This replaces the earlier proposal to select
one output through a menu with no graph connection; that proposal is retained
[historically](../historical/vue_nested_port_clip_scale_and_ruler_input_before_2026-11-16.md#previous-output-selector-project-grid).

A **Coverage input** is the preferred design because it can accept multiple
compatible incoming event contributions. The ruler provides a dropdown to
choose the particular **cover/contribution** whose timestamps are snap marks.
The selected cover is an explicit project-level setting, keyed to stable
connection/cover identity, not the order of a transient materialized Region
list. The compiler and UI must retain enough provenance to distinguish covers
after input fan-in. A Coverage output never becomes a Region input merely
because its current payload contains one cover; existing connection shape
rules remain intact. The input may be implemented through a dedicated
project-owned node if that best fits normal graph ownership. Such a node must
not create a second independent snapping-data model.

Every registered event type has a meaningful **timestamp-only projection to
`TriggerEvent`**: preserve each occurrence's global sample position and drop
the payload. This is a specific event-to-trigger conversion, **not** a claim
that arbitrary event payload types can all convert to each other. The input
may use this projection to accept event streams of different payload types;
if the execution/conversion planner cannot yet preserve this contract, that
is a dependency to implement, not permission to treat event values as times.
The grid need not require a regular beat spacing or a built-in tempo map.

Use bounded viewport-relevant graph/observation queries and coherent versions
to populate the ruler and point-snap candidates. An unrecorded live stream
cannot promise unavailable past or future events; absence of available marks
must be presented truthfully. The always-on nearest-project-sample grid is
the fallback and minimum granularity of authored point positions. Snapping to
one selected event cover is an explicit edit; moving the connected stream or
switching covers never silently changes already stored points. The exact
bypass modifier, snap radius, and transition when a cover disappears remain
implementation decisions. The corresponding curve-editor requirements are in
[Vue Presentation Refinement](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).

### Automation point placement

Every point drag and creation operation quantizes to integer project samples
**while the pointer moves**, not only on release. When a connected ruler cover
is selected and its marks are available, the additional snap rule selects
among those sample indices. Without one, it uses nearest-sample positions.
The curve's vertical bounds and `nano`/`mini`/`editable` rendering levels are
specified in
[Explicit value bounds and lane zoom categories](../vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories).

## Vue presentation refinement

From [Vue Presentation Refinement](../vue_presentation_refinement_direction.md).

The top timeline ruler's **fixed-height header** exposes a graph-connectable
**event snapping input**. This is a project-owned snapping connection, not a
client-side selection that merely watches one arbitrary output. The preferred
input shape is **Coverage**, so multiple compatible event producers can connect
to the ruler; the ruler exposes a dropdown to choose the particular **cover /
source contribution** whose event timestamps are the current snap targets.
The selector must preserve source and cover identity even when several
contributions overlap. Do not union all event timestamps into one anonymous
stream and then present an illusory per-cover selector. Keep the header's
existing non-resizable height and its ordinary port/wire hit targets.
A small dedicated graph node may own the exposed input and selection if that
fits the established project-graph model; the exact ownership is an
implementation design decision, not a reason to invent an unrelated UI-only
source registry.

**Every event payload type can project to a trigger occurrence at the event's
existing global timestamp.** The snapping input accepts event streams through
this explicit event-to-`TriggerEvent` projection, discarding payload without
inventing a new time; this does **not** imply universal conversions among
arbitrary event payload types. The runtime must distinguish multiple covers
within Coverage and perform bounded queries for marks relevant to the viewport
and pending drag. Live/non-replayable producers only supply marks actually
available through their declared observation/execution path; the ruler must
not claim unlimited future event history. No event source means sample-grid
quantization only. The selected cover adds snapping positions without silently
repositioning already-authored control points. Snap distance, bypass modifier,
and what happens when a connected cover becomes unavailable remain choices for
the end-to-end implementation. The previous output-selector-only plan is
preserved [historically](../historical/vue_nested_port_clip_scale_and_ruler_input_before_2026-11-16.md#previous-output-selector-project-grid).

For the ruler-header snap input, test multi-source covers, the selected cover,
timestamp projection from different event payload types, and unavailable
window requests before claiming graph-authored snapping is complete.

## Vue client implementation stages

From [Vue Client Implementation Stages](../vue_client_implementation_stages.md).

4. **Add the top-ruler header's event snap input.** After basic sample-
   aligned editing, expose a **non-resizable header input** connected to graph
   event streams. Prefer a Coverage input with multiple incoming contributions
   and a dropdown selecting the specific cover to use for snapping. Define
   timestamp-only projection from every event payload type to `TriggerEvent`
   without inventing conversions between unrelated event payloads. Preserve
   selected-cover identity, bounded graph queries, unavailability reporting,
   editing history, and project sample-grid fallback. This is a whole
   graph/connection-planning, observation, ruler, and automation feature.

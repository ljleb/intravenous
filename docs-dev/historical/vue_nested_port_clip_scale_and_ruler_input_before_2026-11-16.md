# Superseded Port Grouping, Project Snap Source, And Event Projection Notes

_Status: historical, preserved 2026-11-16 from the `intravenous(20261116-152526)`
repository snapshot before the owner's follow-up on nested inputs, ruler-header
connection, and typed-event-to-trigger projection. Non-normative. The current
replacements are in [Vue Presentation Refinement](../vue_presentation_refinement_direction.md),
[Node Presentation And Manual Controls](../node_presentation_and_manual_controls_direction.md),
[Project Playback And Presentation Grids](../project_playback_and_presentation_grids_direction.md),
and [Cyclic Automation And Event Projections](../automation_and_suspendable_coverage_direction.md).
The preserved passages below are excerpts of the source documents, not new decisions.
Relative Markdown links within excerpts were adjusted to resolve from this archive directory._

## Previous folded-port slot geometry

_Source: `docs-dev/node_presentation_and_manual_controls_direction.md`,
"Proposed port area and temporary expansion", 2026-11-16 snapshot._

The owner settled the grouping rule on 2026-11-13. Input and output controls
always occupy separate locations. For each side, one port has a direct handle;
more than one port uses a grouped slot. A side with no ports has no handle.
Hovering the grouped slot immediately exposes all ports on that side in a
temporary panel over the display area. This does not require Shift, increase
lane height, or depend on how many icons could fit beside one another.

The first port circle occupies the grouped icon's position, using the same
first-item alignment as a connection chooser. The list has no decorative
header or connection summaries beneath its ports. It contains only the port
rows. Near the bottom of the viewport, the remaining rows grow upward while
the first port remains at the opening control. Port details belong in the
contextual status bar or an explicit on-demand detail view, not a default
hover tooltip on every port. The preceding layout is retained in
[Vue Port Lists Before First-Item Alignment](./vue_port_lists_before_first_item_alignment_2026-11-13.md).


## Previous output-selector project grid

_Source: `docs-dev/project_playback_and_presentation_grids_direction.md`,
"Proposed grids from node outputs", 2026-11-16 snapshot._

## Proposed grids from node outputs

On 2026-11-13 the owner proposed selecting a node output as the project's
presentation grid. A grid supplies visible reference positions and optional
snap targets. This proposal does not introduce extra graph ports on the root
node or make the selected output a DSP connection to the presentation.

For example, a node can produce events at beat positions. The user selects
that output as a grid source. The presentation draws marks at the event
timestamps and can snap an edited element to those positions. Irregular
grooves, changing subdivisions, and nonmusical markers use the same idea.
The client does not need to infer a BPM from the spacing.

Any node can provide the source if an observation operation can project its
output into meaningful timeline positions. The source need not be a builtin
beat generator. Arbitrary output values do not automatically define positions:
the selected operation must distinguish event timestamps from coordinates
stored inside payloads or continuous values.

Selection identifies the semantic node, output port, observation operation,
and its parameters. It follows the existing output-observation contract,
including stable source identity, generation changes, and bounded queries.
It does not retain a pointer into a current compiled graph.

Selecting a grid does not change authored output retention or graph values.
Observation can require capture or evaluation work through the normal
`Observations` and GraphJit coordination. The viewport requests only the
positions it needs. A live ephemeral output does not promise past or future
marks that its observation operation cannot produce.

Snapping is an explicit editing action. Later grid updates do not move
previously placed elements automatically. A missing or unavailable source
does not silently become an invented periodic grid.

The context-menu phrase "use as project grid" is illustrative. Grid labels,
multiple grid sources, scope overrides, and behavior during a changing snap
gesture remain open. The general feature follows the first concrete output
observation and editing requirements. It does not block the initial playhead.

### Automation point snapping to the graph-authored grid (owner refinement, 2026-11-16)

The automation curve editor is an explicit early consumer of this grid concept.
When **optional graph-authored snapping** is active, moving or adding
`CurvePointEvent` control points snaps their sample indices to positions
selected from a graph-produced stream of trigger events (or an explicit
timestamp-projecting observation of an
appropriate output). This permits arbitrary graph-authored snapping rules:
a regular beat grid is only one possible source. The client obtains relevant
marks through bounded observation requests; it must not materialize an entire
event history to edit one visible region. Snapping makes an explicit point edit
and does not retroactively move points when the source later changes.

Regardless of whether a project grid is selected or available, automation
points **always** use the nearest project sample position as their minimum
placement granularity, including during live drags. The optional event grid
can only select among those positions; it must not postpone quantization until
release or reintroduce fractional-second authored point times.

The exact snap distance, bypass modifier, precedence of multiple sources, and
unavailable-source interaction remain implementation choices. See the
[accepted curve editor gestures](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping)
for Shift+click deletion, point context menus, right-click creation, and
editable tension handles. These requirements do not assert that grid queries
or point snapping are already implemented.

## Previous automation-grid reference

_Source: `docs-dev/vue_presentation_refinement_direction.md`,
"Automation editing and graph-authored snapping", 2026-11-16 snapshot._

The point editor must **snap to project-selected grid positions** when snapping
is enabled. A grid is derived from a selected graph output's stream of trigger
(or equivalent timestamp-projected) events, **not necessarily a regular beat
spacing**. The same mechanism must support arbitrary author-generated snapping
rules. Query bounded, relevant marks through the existing output-observation
path. If the source is absent or unavailable, do not invent periodic ticks.
A snapped point edit is still an explicit edit; later grid changes do not
silently move saved points. The exact modifier to temporarily bypass snapping,
snap distance, and precedence among multiple grid sources remain open for the
concrete editing implementation.


## Previous optional curve-to-trigger conversion wording

_Source: `docs-dev/automation_and_suspendable_coverage_direction.md`,
"Event types and explicitly meaningful conversions", 2026-11-16 snapshot._

Potential `CurvePointEvent` projections are:

- `SampleEvent`: the timestamp and stored value, discarding segment policy;
- `TriggerEvent`: a point occurrence, discarding value and segment policy;
- `BoundaryEvent`: the outgoing active/inactive state at that timestamp
  (`hold`/`bezier` versus `empty`), discarding value and tension. This may
  include repeated begin/end states at consecutive points. Computing a
  *minimal* active-region boundary sequence requires sequence-aware
  coalescing; a per-event conversion must not claim to do that by itself.

These are meaningful candidates, not mandatory conversion edges until their
semantics and consumers are established. No curve-to-MIDI or MIDI-to-curve
conversion is implied.

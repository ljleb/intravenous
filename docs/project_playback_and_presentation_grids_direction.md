# Project Playback And Presentation Grids

_Status: intended playback contract following the owner's agreement on 2026-11-13.
The generated-grid section records a proposed presentation feature. Neither
section claims implementation. The owner selected a builtin callback structure
for node access to playback state. Ordinary clip sources do not require it._

Related contracts are [Global Indexing](./global_indexing.md),
[System Audio Devices](./system_audio_devices_direction.md), and
[Vue Output Observation](./vue_output_observation_direction.md).

## Project position and display units

The backend owns one playback position for the project session. A playhead is
the presentation of that position. Scrolling or zooming a viewport does not
change playback position. Multiple clients observe the same backend position.

Seconds and project sample indices are two displays of that position. Seconds
use the project sample rate, not the hardware device rate or a port's pace.
At 48,000 project samples per second, sample index 48,000 is one second.

The initial playback controls select integer project sample positions through
`GlobalIndex`. This does not remove fractional coordinates from node execution
or graph data. JSON carries exact sample indices as signed decimal strings,
as required by Global Indexing. A floating-point seconds value is a display
conversion, not a replacement for the exact index.

No built-in tempo map is required for this contract. The interpretation and
display of beats remain a separate presentation question. Automation clips
show their own ruler with seconds and integer project sample positions by
default; their authored point times are sample-aligned during editing, not
free-form fractional seconds. Reuse the enclosing timeline transform and show
the sample grid when zoom makes those divisions readable. See the
[automation clip ruler contract](./vue_presentation_refinement_direction.md#automation-clip-ruler-and-sample-accurate-editing).

**Later macro integration (not an implementation change):** when graph macros
are implemented, they should be able to read and command the session playhead,
seek, and playback state through the appropriate transport boundary. Seeking
or navigating a viewport is not a persistent project graph edit and should not
create a project undo entry merely because a macro requested it. See
[Module Interoperability And Graph Macros](./module_interoperability_and_graph_macros_direction.md).

## Playback and continuous Tick execution

Playback has running and paused states. While running, the project playhead
advances with execution. Pause holds the selected project position. Resume
continues from that selected position. Seek selects a new project position.

Tick execution continues while playback is paused. Audio-device servicing,
live node processing, and Sequential history do not stop with the playhead.
Resume or seek can change the global coordinate of subsequent execution,
as specified by Global Indexing. Neither operation implicitly clears node
state or Sequential history.

### Background clip data during pause

An ordinary audio-clip source can produce its data through a Tock/persisted
output. It does not need live playback state to publish the clip's regions.
The published data remain available independently of playback position.

For example:

```text
AudioClip                         Gain                         SystemAudioOutput
Tock/persisted Sample output ---> Tick Sequential input -----> Tick input
```

The runtime selects the published data window for the Tick input. While paused,
that input receives its own declared `neutral_value`. Background event delivery
provides no events. Tick Region/Coverage views are empty. Stored clip data and
background computation are not erased or globally disabled by this rule.

These are existing requirements in
[Graph JIT Direction](./graph_jit_direction.md)
and [Sequential Playback And Persisted-Data Availability](./coverage_and_background_evaluation.md#sequential-playback-and-persisted-data-availability).
Live Tick sources continue processing. Pause must distinguish background data
delivery from current Tick delivery rather than mute the whole graph.

Persistence alone does not identify background playback. A Tick/persisted
output can record a live source and still supply current Tick data. Its
current Tick delivery must not stop merely because the output is persisted.

### Optional node access through callback state

The selected position and the current Tick callback's global interval are
different facts while paused. The implementation must not treat the count
of rendered device frames as the paused project position.

The owner selected a builtin, read-only playback structure in callback context
when a node needs to inspect playback state. A possible use is a live Tick
sequencer that must react to a playback start. An explicit graph connection
from a new playback-source node is not required.

This capability is separate from the runtime's paused-delivery rule. The clip
example above does not justify adding it to ordinary Tock callbacks or making
it a prerequisite for the initial playhead. Exact member names, callback scope,
and replay semantics must follow a concrete node requirement. A Tock clip
source continues to use requested coverage, not a live playback cursor.

The superseded clip example and recommendation for an explicit source node are
preserved in
[Playback Discussion Before The Initial Contract](./historical/playback_design_discussion_before_contract_2026-11-13.md#initial-contract-draft-before-correcting-the-clip-example).

## Backend ownership and realtime boundary

The intended execution owner is `GraphExecutor`. It applies playback commands
at Tick boundaries and owns the applied position and running/paused state.
`SystemAudioDevices` supplies output-clock pacing and device buffers.
`ProjectGraph` continues to coordinate graph revisions. A position change
does not by itself request graph reconstruction or reset a generation.

Commands from clients have one backend order. Accepted commands reach the
execution owner in that order. Position snapshots describe applied state,
so the client can distinguish an accepted seek from its execution boundary.
Command acceptance does not depend on a client receiving every snapshot.

The audio thread performs no network I/O, JSON serialization, allocation,
or blocking synchronization for playhead reporting. It publishes bounded
state for a control-path reader. Position and playback state must belong
to the same coherent snapshot.

A client can animate between snapshots while playback runs. Backend updates
correct that animation. A pause or seek starts a new position segment, so
animation must not extrapolate across that change. The displayed execution
position does not claim exact hardware speaker time.

The first implementation includes a position/state read and playback commands
through JSON-RPC. Exact route names and update frequency follow the complete
client feature. These controls do not depend on implementing every output
observation operation first.

## Project ruler input for graph-authored snapping (accepted 2026-11-16)

The **fixed-height header of the top project time ruler** exposes an **event
input**. A graph connection to this header configures the time marks used for
presentation snapping, particularly automation control-point editing. Its
header need not be resizable. This replaces the earlier proposal to select
one output through a menu with no graph connection; that proposal is retained
[historically](./historical/vue_nested_port_clip_scale_and_ruler_input_before_2026-11-16.md#previous-output-selector-project-grid).

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
[Vue Presentation Refinement](./vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping).

### Automation point placement

Every point drag and creation operation quantizes to integer project samples
**while the pointer moves**, not only on release. When a connected ruler cover
is selected and its marks are available, the additional snap rule selects
among those sample indices. Without one, it uses nearest-sample positions.
The curve's vertical bounds and `nano`/`mini`/`editable` rendering levels are
specified in
[Explicit value bounds and lane zoom categories](./vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories).

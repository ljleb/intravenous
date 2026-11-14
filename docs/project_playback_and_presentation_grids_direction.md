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
display of beats remain a separate presentation question.

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

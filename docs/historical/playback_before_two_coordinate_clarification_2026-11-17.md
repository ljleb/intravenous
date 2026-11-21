# Playback semantics before the two-coordinate clarification (2026-11-17)

_Status: non-normative historical preservation of **Git-staged-index** documentation,
not the unstaged working-tree changes. Superseded by
[Project Playback And Presentation Grids](../project_playback_and_presentation_grids_direction.md)
and related current documents. The owner clarified the intended behavior during
2026-11-17 discussion. The removed excerpts below are copied verbatim from the
pre-clarification index; they are retained for provenance, **not** implementation
instructions._

This revision replaces the automatic paused neutral/empty input policy, unconditional
seek relocation, a single global finalized output range across coordinate seams,
and unconditional whole-finalized-window recording eligibility. It also clarifies
unprepared-background handling as a deadline failure, not ordinary missing Coverage.
The staged source and its original implementation checkpoint are preserved separately:
this documentation correction does not claim to have implemented the revised design.

## project_playback_and_presentation_grids_direction.md — original project playback contract

Original pre-clarification introduction, position, playback, paused-delivery,
node-access and backend ownership sections (excluding unchanged ruler section):

````text
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
````

## global_indexing.md — transport coordinate correction

Original superseded excerpt:

````text
Tick is an ordered sequence of invocations. Normally one invocation's global interval
ends where the next begins. A pause/resume, seek, loop, or other transport seam may
instead make the next `FractionalGlobalIndex` jump to any global coordinate. The Tick
graph continues running while playback is paused; on resume it observes the selected
global coordinate immediately.
````

## coverage_and_background_evaluation.md — paused delivery and input coverage

Original superseded excerpt:

````text
While playback is paused, background-produced Sequential continuous inputs are treated
as missing and therefore read their own `neutral_value`; background-produced
Sequential event inputs are empty. Region and Coverage inputs expose empty extents.
The Tick graph itself continues executing, including live and ephemeral signal paths.
Resume selects the applicable immutable background snapshot at a callback boundary and
may jump the shared node-visible `FractionalGlobalIndex`; it does not reset live
Sequential history or introduce a second node time coordinate.
````

## coverage_and_background_evaluation.md — recording finalization and seam

Original superseded excerpt:

````text
For `tick_block()`, the disposition applies to the positions the addressed callback
block finalizes: for a block beginning at `B` with `N` positions and an output latency
`L`, `[B - L, B + N - L)`. Those are the positions no later block can change: the block
may still revise sample values or add events in its latency window `[B - L, B)`, and
leaves its own newest `L` positions open to the next block. `written` publishes one
authoritative overwrite of that range and `voided` publishes one authoritative erase of
it. Scalar `tick()` has the same rule with a one-sample block. History does not extend a
recording item. Recording start and stop precision therefore follows Tick callback
boundaries, shifted by the output's latency. (Newly settled 2026-11-10, human
co-author; the superseded text, under which the item covered the callback block itself,
is preserved in
[historical/persisted_recording_callback_block_2026-11-10.md](historical/persisted_recording_callback_block_2026-11-10.md).)
````

## graph_jit_direction.md — GraphJit paused input lowering

Original superseded excerpt:

````text
Playback-paused background data do not freeze Tick or make the audio thread consult
Tock. A Sequential continuous input whose source is background-produced reads its own
`neutral_value`; a Sequential event input reads an empty block. Region/Coverage inputs
are empty while playback is paused. On resume, the next Tick callback adopts the
selected background snapshot at the ordinary callback boundary and observes the new
global coordinate, while its live Sequential state remains invocation-contiguous.
````

## output_capabilities_and_background_demand.md — output demand underrun policy

Original superseded excerpt:

````text
A Tock Region or Coverage output (including sparse automation) supplies prepared
bounded windows to a Tick consumer's Sequential input. Circular output
coverage may contain gaps; the consuming Sequential input applies fallback only
at known-absent positions. Known-covered but unavailable data are an underrun,
not absence. The background actor prepares a successor graph's first window
before activation. If an active graph outruns preparation, the affected
playback block is silenced/reported; no realtime allocation, blocking,
random-access discovery or Tock invocation is permitted.
````

## realtime_background_execution_and_queues.md — queue recording output disposition

Original superseded excerpt:

````text
Disposition is whole-callback-block semantics for Tick/persisted output. A written
`tick_block()` invocation overwrites its complete addressed block; a voided invocation
erases that complete block; an untouched invocation publishes no item. Scalar `tick()`
uses the same rule for its one-sample block. History and latency affect the mutable
Tick storage window but do not expand a recording item beyond the addressed block.
````

## sequential_port_storage_planning.md — sequential port record start stop

Original superseded excerpt:

````text
This deliberately makes recording start/stop precision follow Tick callback boundaries;
sparse event payloads remain sparse inside their authoritative written block.
````

## project_playback_implementation.md — staged implementation plan intro

Original superseded excerpt:

````text
Implement one coherent path: `GraphExecutor` applies ordered session commands
at Tick boundaries, distinguishes held project position from continued Tick
execution, prepares background windows, and supplies neutral/empty background
inputs while paused. Live Tick processing and history continue. RPC exposes
````

## project_playback_implementation.md — staged implementation order

Original superseded excerpt:

````text
2. Implement ordered command application, coherent bounded snapshots, continuous
   Tick coordinates during pause, background-delivery suppression and resume/
   seek preparation through the existing executor boundary.
````

## project_playback_implementation.md — staged implementation checkpoint status

Original superseded excerpt:

````text
`TickInvocationFrame` now selects neutral Sequential and empty Region/Coverage
background inputs while paused. `LiveTickCoverageState` prepares running and
paused coverage together; `TickMaterializationSnapshot` retains both metadata
selections so pause does not recompute coverage on audio. Review paired
coverage's discovery and initial-state behavior before declaring this complete.
````

## project_playback_implementation.md — staged checkpoint remaining

Original superseded excerpt:

````text
Remaining work: finish the client path and controls, regression tests, source
review and owner verification. The active UI is `GraphEditorProvider` /
````

## system_audio_devices_direction.md — device deadline contract

Original superseded excerpt:

````text
`SystemAudioDevices::output_render_loop()` supplies the clock for
`GraphExecutor::activate_pending()` and `GraphExecutor::tick_block()`. The exact pacing
policy is an execution detail, but hardware-device synchronization must not leak into
`ProjectGraph`, `NodeInstances`, or `GraphConnections`.
````

## README.md — docs index

Original superseded excerpt:

````text
- [Circular Global Timeline Indexing](./global_indexing.md)
````

## tick_input_union_and_feedback_direction.md — previous future renderer wording

````text
Any future deterministic offline Sequential renderer is a separate feature. It
may eventually execute feedback from initialized state; that does not change
ordinary background replay eligibility.
````

## coverage_and_background_evaluation.md — earlier no-wait rule

Original pre-clarification excerpt:

````text
The audio thread
never waits for a background evaluation transaction, invokes `tock_coverage()`, or allocates
persisted data on demand.
````

## realtime_background_execution_and_queues.md — earlier no-wait rule

Original pre-clarification excerpt:

````text
Realtime execution must not allocate dynamically, wait for the background worker,
resize cross-thread storage, reclaim heap objects, or destroy superseded immutable
versions.
````

## graph_jit_direction.md — earlier no-wait rule

Original pre-clarification excerpt:

````text
The audio thread never waits or recomputes missing data.
````

## random_access_port_data_direction.md — earlier no-wait rule

Original pre-clarification excerpt:

````text
- the audio thread does not block on background evaluation;
````

# Project Playback And Presentation Grids

_Status: normative project transport and foreground/background binding contract.
The initial playback design was agreed on 2026-11-13; the owner clarified and
replaced its paused-delivery, coordinate-relocation, recording and readiness
semantics on 2026-11-17. This is intended behavior, not a claim that the staged
playback implementation already realizes it. The owner accepted the Timeline
node design on 2026-11-19._

Related contracts are [Global Indexing](./global_indexing.md),
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md),
[Sequential Port Storage](./sequential_port_storage_planning.md),
[Output Capabilities](./output_capabilities_and_background_demand.md), and
[System Audio Devices](./system_audio_devices_direction.md).
The superseded paused-suppression and recording interpretations are preserved
[historically](./historical/playback_before_two_coordinate_clarification_2026-11-17.md).

## Project position and display units

The backend owns one applied playback position for the project session.
The **background playhead** shows that position. The **foreground playhead**
shows the foreground Tick execution position. Scrolling or zooming a viewport
does not change either position. Multiple clients observe the same backend
positions.

Seconds and project sample indices are two ways to display these positions.
Seconds use the project sample rate, not the hardware device rate or a port's pace.
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

## Transport and one continuous foreground execution history

There are **two project-coordinate trajectories**, not two independently
executing node graphs:

- The **background playhead** selects project-time background content. It
  advances while running and holds its applied position while paused.
- The **foreground playhead** gives the starting global index of the next Tick
  block. Each Tick invocation derives its node-visible `FractionalGlobalIndex`
  (`ctx.index`) from that coordinate. The foreground playhead advances during
  successful foreground execution even when paused. Explicit relocation can
  change its next global coordinate.

While project playback runs, both trajectories are aligned. During a pause,
load or reset can link the two positions. A manual foreground hold fixes its
position until it resumes or the graph resets. A third, compiler-owned **physical Sequential cursor** orders
actually executed values and addresses retained rings/history. It is not a
node-visible time, and it continues contiguously across global-coordinate
jumps. The physical audio-device clock is likewise not another project
cursor. See [Global Indexing](./global_indexing.md#sequential-execution-order).

Each new project session opens with both playheads paused at sample index zero
and selects that index as the start. Every explicit seek sets the selected start
when applied, including seeks during playback. Running Tick blocks advance the
background playhead independently of the remembered start. Play/Pause returns
to that start when stopping, and the next Play begins there. The selected start
belongs to the shared session. The previous startup and pause behavior is
[preserved historically](./historical/playback_before_return_to_start_2026-11-17.md).

The following is the semantic command table (the command becomes effective at
a Tick boundary, possibly after preparation):

| Action | Background playhead | Foreground playhead |
| --- | --- | --- |
| Running Tick block | Advances with executed block | Advances with executed block, aligned |
| Pause with Space | Returns to the last selected start and holds there | Continues advancing on executed blocks |
| Seek during a project-only pause | Changes to the destination | Unchanged by the seek |
| Hold with Shift+Space | Returns to the last selected start if running. A paused playhead stays in place. | Stops at its current position |
| Seek during a manual foreground hold | Changes to the destination | Stays at its held position |
| Seek while paused after load or reset | Changes to the destination | Follows the background playhead and stays paused |
| Resume the foreground with Shift+Space | Stays paused | Resumes Tick from its current position |
| Reset Graph State while background is paused | Stays at its current position | Moves to the background position and stays linked until execution resumes |
| Reset Graph State while background is running | Continues playing | Continues playing in alignment with fresh graph state |
| Resume/start | Advances from held position | Relocates to held position, then advances |
| Seek while running | Relocates to destination, continues | Relocates to destination, continues in alignment |
| Explicit foreground relocation while paused | Unchanged | Relocates the one shared foreground coordinate |

Pausing, seeking, resuming, and explicit foreground relocation never
implicitly clear Tick `State`, Sequential history, delay/feedback state, or
physical storage cursors. From the perspective of an operation that does not
inspect `ctx.index`, successive successfully executed Tick values form one
continuous history; global-coordinate segments are joined at transport seams.
Each port still projects the selected global interval onto its resolved pace
lattice. The same shared relocation applies to **every** foreground Tick node:
there are no per-node, per-recorder, or independently advancing foreground
project cursors. While running, a requested foreground relocation must also
relocate the playhead (or be rejected as a separate operation), rather than
breaking alignment.

A counter-based noise generator using `ctx.index` keeps producing changing
noise during an ordinary project-only pause. A microphone, live instrument,
delay or reverb likewise continues ordinary Tick execution during that pause.
Shift+Space holds all foreground callbacks without resetting their state.
Live observations report no current capture while held. Recorded and other
position-addressed observations continue to use background queries.
Reset Graph State can prepare and activate fresh state while held. Starting
project playback with Space resumes both playheads. A transport-aware node can
choose to silence its own contribution or otherwise respond to transport;
this is **authored node behavior**, not a global rule for Sample inputs.
Similarly, smoothing is an explicit signal/input interpretation rather than a
mandatory approximation for every parameter change; the compiled graph should
preserve sample-exact authored behavior.
[Earlier pause contract](./historical/foreground_playback_before_foreground_hold_2026-11-26.md)
[Earlier foreground hold rule](./historical/playback_before_foreground_hold_in_place_2026-11-27.md)

### Reset graph state

_Owner direction, 2026-11-25._

Shift+R invokes **Reset Graph State** for the whole graph, regardless of
selection. Prepare a fresh execution generation with its own `NodeStorage`
and `TockStateStorage` while the current graph continues rendering. Construct
fresh `State` objects and run their `initialize()` hooks during preparation.
Reset input and output port history, still-revisable writes within output
latency windows, and in-flight Sample and Event deliveries, including detached
connections. Use the graph's fresh-start semantics, including any authored
initial values in a detached Sample delay.

Publish the initialized generation at an ordered audio block boundary. The
cutover transfers no node state, history, or pending latency writes from the
predecessor. Run predecessor `release()` hooks at the boundary required by
their execution owner and reclaim storage after both executors finish with it.
The owner accepts temporary overlap of the two runtime allocations. For two
similarly sized generations, allow roughly twice the node and port storage,
plus any extra history required by the startup plan. Preparing the new state
ahead of the cutover moves allocation and initialization off the audio thread.

The immutable startup artifact can be compiled ahead of the reset and reused
while its compilation inputs stay current. It uses empty-predecessor startup
semantics and has its own lifetime independent of predictive cutover code.
Each reset still prepares fresh mutable storage.

Preserve the authored project, including controls, connections, and saved
recordings. Keep the applied background playhead and align the foreground
playhead to it at the boundary. A graph transition must start from state after
the reset if it activates afterward. The session action is the one reset
control for this feature.
[Earlier reset designs](./historical/runtime_reset_before_whole_graph_shortcut_2026-11-25.md).

The old graph can keep recording while the replacement prepares. At the cut,
close the old producer queues. The background executor still commits records
that were finalized and queued before the cut. When the replacement's Tick
input preparation depends on one of those recordings, hold its first block
until the worker drains the old queues and publishes reconciled input views.
Keep the playhead at the cut during that hold. A buffered output that the
replacement's input preparation doesn't read creates no such hold.

### Background selection, coverage and pause

An ordinary clip or automation source can publish Tock/buffered or computed
position-addressed data without consulting the live playhead. Such data and
its Coverage exist independently of transport state. A module can compose
multiple clips on a lane, including offsets, overlaps and fades, using the
general graph facilities; the executor does not identify or mix "clips" by
special-case type.

For example:

```text
AudioClip                          Gain                       SystemAudioOutput
Tock/buffered Sample output ------> Tick Sequential input ----> Tick input
```

For a **Sequential input** reading position-addressed Sample data, normal
playback selects each traversed project position; pause repeatedly selects
the **held applied playhead position**. If the selected position is covered,
its value remains available. A genuinely uncovered position uses that
**input's** declared fallback/`neutral_value`. Known-covered data not yet
prepared are unavailable, **not** uncovered, and never justify fallback.

This is selection of the input's position, not a global remapping of all
Sequential streams. Current/live Tick input values continue to follow actual
foreground execution. A Region or Coverage **Random Access input** retains its
ordinary prepared, pinned, position-addressed view while paused; it does not
become empty, collapse to the held position, or lose its actual Coverage.

An authored Tick output rule uses the coverage of the values selected for
that evaluation. A paused live invocation can select background data at
playhead position `P`, while pointwise Tick replay requested at global
position `I` selects data at `I`. Output values and coverage may differ
between those evaluations. For a stateful Tick rule, a finite tail follows
the sequence of values delivered to the live callback. See
[Tick Output Coverage And Feedback](./tick_input_union_and_feedback_direction.md)
for the output rules, replay contract, and feedback SCC behavior.

### Event delivery is selection of occurrences, not pausing the Event stream

Background Event Coverage and stored occurrences remain available while
paused, just as Sample data do. Generic Event input machinery does **not**
remove Events merely because playback is paused. Instead, the transport
selection determines which occurrences a Sequential input delivers:

- During running playback, occurrences are delivered once when their project
  positions are traversed in an executed interval.
- A **newly applied seek while paused** selects occurrences at its destination
  and delivers them **once**. Repeating a seek to the same destination is a new
  encounter and can deliver them once again.
- Repeated paused Tick invocations at an unchanged playhead do **not**
  redeliver those occurrences. Merely pausing does not synthesize a seek event.
- A resumed running traversal follows the normal traversal rule, including
  occurrences at the resumed starting position when applicable. A node that
  needs to reconcile a paused audition with subsequent playback can do so as
  authored behavior (for example MIDI voice management).

When the applied playhead and `ctx.index` are different, a selected Event's
original project timestamp cannot be passed unmodified as its foreground
callback timestamp: existing Sequential Event consumers interpret timestamps
relative to the current invocation. The binding must deliver it at the
corresponding local execution position (normally the beginning of the paused
seek's invocation), retaining source position/identity when required by
planning or provenance. It must not rewrite the authoritative stored Event.
Preserve ordering and applicable per-port pacing. Repeated execution at the
held position is not repeated occurrence delivery.

An authored Event gate can deliberately suppress paused audition, just as an
audio-playback node can suppress held Sample excitation. Neither suppression
belongs to the generic port domain. This allows graph-authored clip audition
on paused seeks, including MIDI and automation Events, without requiring a
second Event access model.

### Node-visible playback state

The owner selected an optional builtin, read-only playback structure in Tick
callback context for nodes that actually need transport state. It may expose
`playing()` and, when a concrete consumer requires it, a transition/segment
indication. Do **not** add a held-playhead-position accessor to every Tick
node: node-global `ctx.index` remains the one position available to ordinary
Tick code, and no requirement discussed so far needs a second node-facing
time coordinate. An optional transport-aware callback need not be used by
ordinary clips, gain, or CBRNG nodes. Tock callbacks use their requested
coverage, not the live session cursor.

## Buffered Tick output authoring and transport seams

`Sequential/buffered` output is the existing recording facility. The executor
does not prohibit writing merely because playback is paused. An authored
recorder's Record/Arm/Stop policy selects whether to write, `write_void()`, or
leave the output `untouched`; monitoring or producing a value is not
synonymous with committing a recording. There is no universal recorder-arm
state, no new recorder port kind, and no per-recorder execution cursor.
A recorder can deliberately write only while running, or can continue writing
at the independently advancing foreground global coordinates while paused.
An explicit foreground-relocation command can align those coordinates with
the held playhead before recording; this relocates **all** Tick nodes. Multiple
recorders share this coordinate, even when recording separate streams.

### Sampler recording files

_First implementation, pending build and runtime verification._

The built-in `Sampler` takes stereo `audio` and a `record` gate. At the start
of each callback block, a gate of at least `0.5` selects recording. The Tick
callback copies into its project-rate `Sequential/buffered` `recorded` port.
An unarmed callback leaves the output `untouched`. Sequential consumers then
read the pinned recording at the selected playback positions, with silence
in gaps. Newly written samples take effect in that callback and are also
recorded on finalization. Waveform observations and Random Access consumers
use the published recording from the same port. The
[earlier live-output behavior](./historical/sampler_live_output_2026-11-25.md)
delivered silence during unarmed callbacks.

For a project-authored Sampler, its Settings popover edits the project-relative
recording directory through the constructor argument.

`DeclarationContext::record_output_to()` declares the output and its
project-relative directory. `GraphExecutor` resolves a recording binding from
`ResourceContext` for the output's producer route. After the background
transaction commits, it gives the disk worker the finalized interleaved frames
at their project positions. The worker writes batches about every 100 ms and
updates the float32 WAV header. Files live in a uniquely named take directory
under the configured path, which defaults to `recordings`. Each part's name
contains the signed project sample index of its first frame.

A contiguous write extends its part, a position inside a part overwrites those
frames, and a gap starts another part. Adjacent parts produced by filling a gap
keep their filenames in this implementation. A WAV size limit or the signed
sample seam also starts a part. Reading or replaying the buffered output issues
no file writes. Closing the host writer drains its committed batches. Reopening
the project keeps the WAV assets on disk. Loading them into the buffered port
of a new session remains future work.

### Finalized recording positions

Recording semantics are based on **actually authored eligible positions** and
on the physical Sequential output-history finalization rule. Every position
actually executed and eligible for recording is eventually represented by its
finalized value (or an explicit `voided` edit), not silently lost because a
later invocation disarms recording. The same rule applies to authoring while
paused. The default meaning of `untouched`, `written`, `voided`, and explicit
empty Event writes is unchanged. In particular, producing default silence in
a skipped Tick node is an actual authored write when it uses the output facade;
it is not equivalent to `untouched`.

A Tick block with a buffered Sequential Event output can use
`push_and_append_to_recording_gaps(event, sample_offset)` to fill gaps in its
recording. The `append_events_in_gaps` disposition applies only to Event
outputs. The background writer compares the finalized range with the
recording as it stands, including earlier records in the same transaction.
The writer preserves previously recorded positions, including positions
that contain no events. The current Tick event stream still reaches live
consumers. Click Detector uses this to retain the original detections when
playback revisits a recorded range.

A block with output latency `L` can revise previously authored values during
later physical Tick invocations. Finalization follows this **physical**
invocation order. The project addresses belong to the corresponding global
coordinate strips, not to the numerically contiguous interval obtained by
subtracting `L` from the current block's `ctx.index` after a seek. For example,
with two samples of latency, executing `[100,104)` then jumping to `[500,504)`
can make one subsequent finalized group correspond to positions `102, 103,
500, 501`; it must not be mislabeled `[498,502)`. The recording transport
may need to split one physical finalized group into several project-addressed
ranges. Recording authorization/void/written provenance must survive a
latency window even when the node's authoring policy changes. Seek, pause,
resume and loop seams do not automatically seal, discard, or reset that
pending history.

The simple `[B - L, B + N - L)` finalized range is valid **inside an
uninterrupted global-coordinate strip**, not as a blanket rule for a group
crossing a seam. A separate graph-destruction/termination contract may be
needed for still-revisable values when foreground Tick execution actually
ends. Ordinary pause does **not** create that condition: foreground Tick
execution continues and can finalize outstanding values. A recorder's
eligibility at a position is not retroactively changed when its value
becomes final later.

## Background preparation, pending seeks, and missed deadlines

Background evaluation is a feed-forward DAG. Its dependencies are prepared
and bound before the foreground Tick root consumes them; background execution
does not execute Tock from the audio callback. Feedback SCCs belong to the
Sequential Tick schedule. Ordinary pointwise background replay excludes them.

A seek is a **requested** destination until its required background data are
ready. A running seek retains execution at the old applied position while its
destination is prepared. A return to the selected start likewise keeps the
applied selection while its held position prepares. A resume remains paused
until the required starting window is ready. A paused seek can retain the previous applied
position until its requested destination and its once-only Event audition can
be applied coherently. Only application of the new position changes the
selection consumed by the graph. Superseded pending cursor requests need not
all be applied or auditioned. Control-command ordering and any coalescing
must be defined so the last intended destination is not lost and so an
obsolete request cannot indefinitely block a newer one.

The executor can seal a bounded foreground prefix before an early Tick branch
advances through it. A seek requested after the seal applies after the foreground
consumes the prefix and the destination is ready. The prefix uses the applied
project positions and the consecutive physical execution indices. At the cut,
stateful early branches continue from the state at the end of that prefix.
A node with `static constexpr bool is_live = true` requires foreground time.
`is_live` defaults to false. The requirement follows signal flow through
every downstream node. Every Tick branch outside that set can advance ahead
of playback, including branches with node state or feedback. A branch that
feeds a live mixer can advance ahead if its own inputs allow it. Both branches
use the same applied playback selection.
[Earlier opt-in prototype](historical/tick_resampler_first_proposal_2026-11-26.md#rejected-early-tick-opt-in)

Early execution changes when the worker runs a callback. Given the same
applied inputs and Tick callback ranges, the early and foreground parts
together preserve the Sample values, Events, port delays, and node state
of ordinary foreground execution.

When an ordinary graph edit reaches a run-ahead branch, select one shared
cutover in Tick execution order. The cutover can follow callbacks that already
executed ahead of playback. Each branch uses the old graph before that index
and the successor from that index. Compatible node state, finalized in-flight
values, and pending input and output work follow the existing graph transition
rules at the cutover.
[Port state at cutover](./sequential_port_storage_planning.md#port-windows-are-semantically-node-owned-even-when-storage-is-shared)

An edit can give a previously early Tick branch a live input. If compatible
work admitted before the cutover still needs that input to finish, playback
can wait for the block to become ready. Preserve its samples and their order
when resuming the device output.

The frontend may display a **ghost/requested cursor** immediately at a pending
destination while continuing to display the **applied background playhead**
separately.
Applied position and running state come from coherent backend snapshots.
The applied cursor, not the requested ghost position, identifies which data
were actually selected for execution. No project edit or graph rebuild is
created by merely seeking or drawing a ghost cursor.

A click on the project time ruler seeks in the current transport mode. When a
drag starts, playback pauses and the displayed background playhead follows
the pointer.
The graph holds the last applied position until the drag position is ready.
Releasing the pointer selects the release position as the new start. Playback
resumes from that position if it was running when the drag began. A play or
pause command during the drag sets the state to restore on release.
The editor keeps the release position visible through the transport handoff,
so native cursor return can land on the displayed playhead.

The Beats lane uses the same seek gesture as the ruler. The ruler snaps to its
displayed beat marks in Beats mode. The Beats lane snaps to its own visible marks.
In Beats and Custom, a ruler drag reads a rolling beat window ahead of the
playhead so snapping continues beyond the visible timeline. A drag in a
Beats lane projects that lane's grid at the same zoom beyond its edges.
Newly loaded marks take effect from the current drag position and preserve
any travel already spent on the active detent.
During a drag, a snap adds physical pointer travel at the selected sample.
The samples on either side stay reachable. Mouse drags use pointer lock.
When native cursor return is available, release returns the pointer to the
playhead or to the nearest visible edge of the drag surface.
Object drags and middle-button pan and zoom share the native cursor return.
Each gesture supplies a destination. The graph editor waits for pointer
lock to end before it asks the native helper to move the cursor.

Inside lanes, the playheads are drawn on surfaces with a project time axis, such
as an automation curve or a waveform. Controls and live meters keep a clear
view of their own content.

The project ruler marks the selected start when that position is in view.
The foreground playhead appears as a faint cursor on the ruler and on lane
surfaces with a project time axis when it differs from the displayed
background playhead.

The background scheduler should preserve demand for the **currently applied**
position/window while preparing a different pending destination. Forward
lookahead, bidirectional neighborhoods around a dragged cursor, pointer
velocity/direction, reuse of overlap, priority changes, and cancellation of
obsolete speculative work are tuning strategies to evaluate with actual
projects. Cache occupancy and prediction alter readiness/latency only, never
Sample values, Event occurrences, Coverage, or graph semantics. A cold,
expensive request may not be instant; predictive performance is a target,
not a semantic guarantee.

Missing *prepared* data at a known-covered required position is a missed
execution deadline, not a new transport action, an uncovered input, or
permission to synthesize neutral data. In the chosen failure policy, a
foreground Tick block executes with its complete required prepared inputs,
or it does **not** execute. During an unsuccessful attempt, no Tick callback,
Sequential state, physical Sequential cursor, foreground global coordinate,
applied playhead, or recording output advances. The audio hardware's clock
cannot be stopped by this rule: an audio-device underrun/dropout can occur
outside the authored graph. Device fallback/silence is not graph-authored
sample data and is not a recorded value. Waiting for preparation may miss an
audio-device deadline; this is a resource/scheduling failure to diagnose and
optimize, not intended normal behavior. Live device capture continues in
physical time and obsolete captured frames may be dropped, with no backlog
latency or additional underrun-specific resampling. MIDI note-off/controller
state dropped during a stall may need input-side state reconciliation (or a
clear-all-notes recovery) to avoid stuck voices.

Blocking **foreground DSP execution** must not indefinitely block command
processing, graph activation/replacement, shutdown, or recovery from a
missing/unfulfillable preparation request. The implementation can preserve
the no-Tick-execution invariant with an interruptible readiness check rather
than an uninterruptible wait in the device callback. Recorded data missing
because foreground work has not yet produced them are not magically
prepared by waiting. Capacity failure or permanent missing dependencies need
an explicit failure/reporting path; background DAG-ness alone does not
promise that a request can become ready. A producer may predictably underrun
when configured to record into an expensive dependent computation; this is
not a semantic exception that authorizes substitute values.

A terminal error while preparing a pending executable generation follows the
[ordered cutover recovery contract](./project_graph_application_architecture.md#recovery-from-terminal-preparation-failure).
Playback underruns and retryable preparation failures leave the generation
eligible for activation.

## Future isolated background Sequential rendering (distinct from playback)

A future facility may instantiate a **copy of the reachable Sequential subgraph**
with initialized state, execute it in physical Sequential order over each requested
Region, and materialize the results for position-addressed consumers. This is not
ordinary pointwise contextual Tick replay: it can execute stateful pure processors,
including a Sequential feedback SCC inside the isolated instance, provided all
nodes in its executed dependency closure satisfy the declared purity contract.
The **background evaluation dependency graph remains a feed-forward DAG**; an
internally time-stepped isolated Sequential instance does not create a background
dependency SCC.

Unmarked nodes may promise purity according to the intended trait convention;
`impure` nodes (for example live system microphone inputs) are not re-executed in
these jobs. A published recording can terminate traversal: the job reads those
recorded values without re-executing their impure original producer. Violating a
node's declared contract is an implementation error; automatic proof of arbitrary
DSP purity is not required. Jobs may run in parallel across different Regions,
including overlapping Regions, but each **semantic Region** receives its own
initialized state. Two Regions can therefore produce different values at the same
project position. Preserve Region identities and their state-start boundaries.
Splitting work for scheduling is not permission to insert new state resets.

This facility can regenerate output after graph/input edits and can implement
rendering/freezing or cached procedural materialization. A live
`Sequential/buffered` recording is different: a one-of-a-kind physical input
may not be reproducible and its authoritative recorded data must survive cache
eviction. These operations can reuse code generation, scheduling and storage
mechanisms without being the same execution strategy. **No decision is made here**
to add an arbitrary Sequential input to every Tock node or a new node callback
family; the API that declares and initiates isolated jobs remains future design.
This facility does not move the live playhead or foreground `ctx.index`.

## Backend ownership and realtime boundary

`GraphExecutor` owns command application, the one foreground coordinate,
applied playhead, and execution block boundaries. `SystemAudioDevices`
supplies output-clock pacing and device buffers; `ProjectGraph` coordinates
graph revisions. A position change does not itself reconstruct the graph,
reset a generation, or create a project undo entry.

Client commands have an accepted order. A command may be accepted but not
applied while its required data prepare; snapshots describe **applied**
position, state, and command/segment identity, not an optimistic requested
position. Snapshot publication is coherent and bounded. The audio callback
performs no network I/O, JSON serialization or allocation for playhead
reporting. A client can animate between snapshots while running, but must
not extrapolate across a pause, seek or other segment change. Displayed
execution position does not claim exact hardware speaker time.

The first implementation exposes a position/state read and playback commands
through JSON-RPC. Exact route names, optional foreground-relocation control,
transition metadata and update frequency follow the complete feature
implementation. No output-observation feature, tempo map or generated beat
grid is a prerequisite.

## Timeline nodes and project snapping

_Accepted design, 2026-11-19._

The ruler's top-right popover selects the project-wide snap mode. Its hit
area extends to the top and right edges and left of the visible button.

- **Seconds** is the default. It shows time graduations without a beat grid.
  The graph has no editor-managed snapping nodes.
- **Beats** creates an editor-managed `builtin.beats` and connects its
  `beats` output to the editor-managed `builtin.timeline`. The popover
  exposes the same BPM, beats per bar, and subdivisions as an authored Beats
  lane.
- **Custom** uses the editor-managed `builtin.timeline`. In mini and nano
  layouts its `beats` input shares the playback row when the header can
  fit the socket beside the transport indicator and seconds. A narrower
  header or a taller lane layout places the input on its own row below the
  playback readout. Compatible BeatEvent outputs connect using ordinary
  graph cables. Multiple sources contribute to the same grid. The socket
  uses the same horizontal column as the lane inputs in either placement.

The sticky ruler occludes ordinary graph cables. A cable attached to the
ruler's input and a cable being dragged can draw over it.

[Previous Custom input placement](./historical/custom_beats_input_before_second_row_2026-11-19.md).

The editor owns these internal instances and omits their lanes. Active mode
follows the graph edit that creates or removes them. Switching modes retains
the internal Beats settings and Custom connections for the next time that
mode is selected. Authored Beats and Timeline instances remain in the graph.

In Beats and Custom, observe the resolved `beats` input of the editor-managed
Timeline. Use its receiving-port timing and Coverage. At coincident sample
indices, use the smallest `BeatEvent.depth`. Each viewport selects the marks
at its zoom level and uses the same list for the ruler and snap gestures.

The `beat-grid` window can request `includeIndexedRuns`. In that response,
each run of indexed marks carries its source output and Region identifiers,
Region start and length, event count, depth, first event index, and stride.
The run lists its sample positions once. Marks read by time stay in `marks`.
Subscribers that omit the option receive the flat list of sample positions and
depths.
Order flat marks by forward sample distance from the requested window's
start. At the same sample, list the coarsest depth first. A window that
crosses the signed sample seam keeps its local order. Indexed runs keep
their event indices in source Region order.

The Custom ruler numbers the bars in each indexed depth-zero Region from
its start by default. Recover each bar's event index from the run's first
index and stride, then add one. For a Region that starts at a negative
sample and ends at sample zero, subtract the Region's total event count
as well. Its last bar is 0, and the first bar in a Region starting at
sample zero is 1. This matches Beats when its output drives Custom.
For other Regions, also subtract the event count for bars in the second
half of the sample circle relative to the Region start. Marks read by
time supply lines and snap targets.
Coincident bar marks with conflicting labels share one line without a number.
[Previous Region-only rule](./historical/custom_beat_labels_before_zero_ending_regions_2026-11-23.md) ·
[Earlier sample-zero labels](./historical/custom_beat_labels_before_region_relative_numbering_2026-11-22.md).

The viewport picks marks by a minimum horizontal pixel distance at its current
scale. Closer candidates give way to coarser marks. The Beats preview uses the
same distance rule. A query may include marks beyond the viewport so the
editor can redraw and snap immediately during nearby pans and zoom changes.

The timeline draws the ruler's visible beat marks continuously over each run
of time-aware lanes, including automation lanes. The ruler, lane grid, and
Beats preview emphasize bar lines most, then beat lines, then subdivisions.
Each visible depth uses the minimum pixel gap between guides at that depth
or coarser. Opacity falls off with that gap and the overall minimum gap, so
a dense grid of bars alone also dims. The coarsest displayed depth has the
most emphasis. Guides fade when zoom adds or removes them.
While the viewport changes, the editor can project overlapping data from
the last ready observation window until the replacement arrives.
A point created within the snap radius starts
at the nearest visible mark. During
a drag, use the same pointer-travel detents as value snapping. A beat and a
neighboring point at the same sample share a detent. Highlight the active time
guide in blue. The displayed background playhead at pickup also provides a
detent in all snap modes. Between detents, quantize to the nearest
project sample. A grid change affects subsequent edits. Existing point
positions are retained.
[Earlier radius-only drag snapping](./historical/automation_beat_snap_before_detents_2026-11-19.md).

The [previous ruler input and cover selector](./historical/ruler_coverage_input_before_timeline_nodes_2026-11-19.md)
is preserved with its source-selection rationale.
The [initial authored Timeline design](./historical/ruler_grid_before_managed_snap_nodes_2026-11-19.md)
records the previous source and merge rules.

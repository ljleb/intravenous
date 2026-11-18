# Project Playback And Presentation Grids

_Status: normative project transport and foreground/background binding contract.
The initial playback design was agreed on 2026-11-13; the owner clarified and
replaced its paused-delivery, coordinate-relocation, recording and readiness
semantics on 2026-11-17. This is intended behavior, not a claim that the staged
playback implementation already realizes it. The generated-grid section
remains a proposed presentation feature._

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

While running, both trajectories are aligned. While paused, they are
independent. A third, compiler-owned **physical Sequential cursor** orders
actually executed values and addresses retained rings/history. It is not a
node-visible time, and it continues contiguously across global-coordinate
jumps. The physical audio-device clock is likewise not another project
cursor. See [Global Indexing](./global_indexing.md#sequential-execution-order).

Each new project session opens paused with its background playhead and selected
start at sample index zero. Every explicit seek sets the selected start when
applied, including seeks during playback. Running Tick blocks advance the
background playhead independently of the remembered start. Play/Pause returns
to that start when stopping, and the next Play begins there. The selected start
belongs to the shared session. The previous startup and pause behavior is
[preserved historically](./historical/playback_before_return_to_start_2026-11-17.md).

The following is the semantic command table (the command becomes effective at
a Tick boundary, possibly after preparation):

| Action | Background playhead | Foreground playhead |
| --- | --- | --- |
| Running Tick block | Advances with executed block | Advances with executed block, aligned |
| Pause | Returns to the last selected start and holds there | Continues advancing on executed blocks |
| Seek while paused | Changes to the destination | Unchanged by the seek |
| Remain paused | Holds destination | Continues advancing on executed blocks |
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
noise while paused. A microphone, live instrument, delay or reverb likewise
continues ordinary Tick execution during an intentional pause. The playhead
must not freeze, mute, or reset these computations. A transport-aware node can
choose to silence its own contribution or otherwise respond to transport;
this is **authored node behavior**, not a global rule for Sample inputs.
Similarly, smoothing is an explicit signal/input interpretation rather than a
mandatory approximation for every parameter change; the compiled graph should
preserve sample-exact authored behavior.

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

Coverage derived by `TickReplayMode::input_union` uses the actual coverages of
the values selected for that evaluation. A paused live invocation can select
background data at playhead position `P` while a pure pointwise Tick replay
requested at global position `I` selects data at `I`. Their output values and
coverage may differ even when the same node implementation is intrinsically
replayable. This is not permission to weaken pointwise replay or change its
existing purity/state constraints. Do not mix live transport-selected
coverage with background position-addressed replay coverage, or silently
turn held covered inputs into uncovered inputs. [Tick Input-Union Coverage](./tick_input_union_and_feedback_direction.md)
remains authoritative about fallback, `input_union`, `full`, and feedback SCCs.

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

Recording semantics are based on **actually authored eligible positions** and
on the physical Sequential output-history finalization rule. Every position
actually executed and eligible for recording is eventually represented by its
finalized value (or an explicit `voided` edit), not silently lost because a
later invocation disarms recording. The same rule applies to authoring while
paused. The default meaning of `untouched`, `written`, `voided`, and explicit
empty Event writes is unchanged. In particular, producing default silence in
a skipped Tick node is an actual authored write when it uses the output facade;
it is not equivalent to `untouched`.

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
does not execute Tock from the audio callback. Feedback SCCs belong to
Sequential foreground execution and cannot enter ordinary pointwise
background replay. A proposed future pure, isolated Sequential renderer is
separate from ordinary replay and does not change these rules.

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

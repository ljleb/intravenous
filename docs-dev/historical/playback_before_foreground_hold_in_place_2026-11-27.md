# Playback before holding the foreground in place

Historical snapshot preserved on 2026-11-27 from [Project Playback and Presentation Grids](../project_playback_and_presentation_grids_direction.md). The replacement holds the foreground playhead in place and links the playheads at load and reset.

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
| Hold with Shift+Space | Returns to the last selected start and holds there | Returns to the same position and stops executing |
| Seek while both playheads are held | Changes to the destination | Moves to the destination and stays held |
| Release the foreground with Shift+Space | Holds destination | Resumes Tick from the held position |
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


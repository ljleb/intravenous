# Runtime Reset Before Whole Graph Shortcut

_Preserved on 2026-11-25 from `project_playback_and_presentation_grids_direction.md`
and `implementation_fronts_direction.md`. The owner chose one whole-graph
shortcut in place of selection and per-connection controls. See the
[current reset design](../project_playback_and_presentation_grids_direction.md#reset-graph-state)._

### Explicit node state reset

_Future work recorded 2026-11-21._

Provide a session action that resets the state of all nodes in the current node
buffer. Use the `release()` hook before `initialize()` and reuse the existing
buffer allocation. Apply the reset at a safe execution boundary. Set the
foreground playhead to the applied background playhead position at that boundary.

### Selected node state and connection history reset

_Owner direction, 2026-11-25._

Shift+R resets selected nodes. At a safe execution boundary, call `release()`
where provided, destroy each selected `State`, construct fresh state in the same
`NodeStorage` slots, then call `initialize()` where provided. Keep the current
transport position for a selection reset.

Every connection can reset its retained runtime history. The operation applies
to Sample and Event connections, including those with an authored detach delay.
Use the connection's fresh-start semantics at the reset boundary, including an
authored initial value where the connection has one. A connection reset is a
runtime operation. Preserve the cable, its settings, and saved recordings.

Expose a connection-level reset in the Shift-hover connection popover so a
user can clear one route. Also include the selected nodes' output history and
the resulting retained outgoing deliveries in Reset Selected Node State. This
lets a user clear queued feedback from a node without hunting for the cable.
When a physical ring serves several connections, a reset must preserve what
unaffected branches can read. The runtime owns the reset boundary and any
interaction with an in-flight graph transition.

### Implementation friction for selective reset

`CompiledGraph` describes retained port windows, and `SampleFeedbackTimelinePlan`
identifies the backing representation of a detached Sample branch. A
producer-home timeline may share that backing with the producer. A merged Event
input can retain a window without source indexes. Clearing bytes for one
connection may therefore clear a sibling's data, or the stored input history may
lack the provenance needed to subtract one route. The recommendation was to keep
history the destination already read and reset delivery from the chosen route at
the boundary. Evaluate reset-time transition storage before adding storage or
per-frame checks to steady execution. Regression cases included ordinary and
detached Sample and Event connections, fan-in and fan-out, and reset racing a
prepared graph transition.

### Whole-graph reset before fresh allocation was accepted

_Preserved on 2026-11-25 from
`project_playback_and_presentation_grids_direction.md`. The owner accepted
fresh allocation and temporary overlap to prepare the reset off the audio
thread. See the [current reset design](../project_playback_and_presentation_grids_direction.md#reset-graph-state)._

Shift+R invokes **Reset Graph State** for the whole graph, regardless of
selection. At a safe execution boundary, call `release()` where provided,
destroy each node's `State`, construct fresh state in the existing allocations,
then call `initialize()` where provided. Reset input and output port history,
still-revisable writes within output latency windows, and in-flight Sample and
Event deliveries, including detached connections. Use the graph's fresh-start
semantics, including any authored initial values in a detached Sample delay.

Preserve the authored project, including controls, connections, and saved
recordings. Keep the applied background playhead and align the foreground
playhead to it at the boundary. A graph transition must start from state after
the reset if it activates afterward. The session action is the one reset
control for this feature.

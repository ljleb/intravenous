# Manual-Control Gesture Updates Before Transient Live Values (2026-11-14)

_Status: historical, non-normative. Preserved on 2026-11-14 from
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md)
and [Project Undo, Redo, And Backup Direction](../project_undo_redo_and_backups_direction.md)
when the repository owner accepted transient gesture updates. The replacement
contract is "Live gesture values are transient" in the manual-controls direction._

## Why it was superseded

Each `manualControls.update` was an ordered, acknowledged project mutation. It
rewrote the durable `ProjectManualControl::stored_value` and the gesture's history
entry under `ProjectGraph::mutex_`, advanced the session revision, and made every
editor re-read `graph.getScope`. The client sent one update at a time, each
carrying the preceding revision. New audio targets therefore arrived late and in
bursts, and a storage-changing control compilation held the same mutex and
blocked them entirely. Each late step was ramped over one root call and then held,
so dragging a knob sounded stepped. A concurrent edit by another client also made
the next update fail its `expectedRevision` check, ending the gesture. The
friction record is in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md).

## Superseded text: Node Presentation And Manual Controls Direction

From "Presentation interaction state outlives Vue components":

> For socket RPC clients, the server assigns the interaction's client identity from
> the authenticated connection. A request cannot claim another client's identity.
> Disconnect removes that client's hover and active-interaction state. The durable
> manual value remains the last value accepted before disconnect.

From "Manual-control history is grouped by completed interaction (planned)":

> On disconnect, the last
> accepted durable value remains authoritative; any unfinished history grouping
> must be resolved consistently rather than inventing an audio-thread rollback.

From "Interpolation exists only during active scrubbing":

> A settled manual value is eligible for exact constant specialization. The stored
> manual value is the latest value retained by the project and UI. The effective manual
> value is the value used by the active executable graph. These values can differ while
> a replacement graph compiles.
>
> Hover requests compilation of dynamic-input code. Hover does not activate that code.
> If scrubbing starts first, the UI updates the stored value while the active graph keeps
> using its existing constant. No generic execution path applies the new value to code
> that does not support a dynamic input.

## Superseded text: Project Undo, Redo, And Backup Direction

From "First implementation contract (2026-11-13)":

> Manual-control updates coalesce only while that interaction owns the latest
> history entry. An intervening accepted edit seals that entry; the next control
> update begins a new entry after the intervening edit. Disconnect finalizes the
> last accepted stored value. Undo/redo requires active manual-control interactions
> to finish first; it does not rewind a pointer gesture still being operated.

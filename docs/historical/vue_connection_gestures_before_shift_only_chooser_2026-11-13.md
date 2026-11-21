# Connection Gestures Before The Shift-Only Chooser

_Status: historical proposal and implementation checkpoint, preserved on 2026-11-13._

Sources: `docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`, immediately before implementation.
The owner accepted the gesture rules and removed the proposed right-click menu.
The replacement is [Disconnection And Endpoint Movement](../node_presentation_and_manual_controls_direction.md#disconnection-and-endpoint-movement).
The source checkpoint is [Connection Editing](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

## Previous proposal

### Proposed disconnection and endpoint movement

The owner proposed these gestures on 2026-11-13. They are not implemented yet.
The reference to a left click means a press-drag-release gesture in this
proposal. A quick click alone does not commit a connection edit.

A normal primary drag from a port starts a new connection, including when that
port already has connections. Shift plus a primary drag selects an existing
connection and moves the endpoint under the pointer. Its other endpoint stays
fixed. For `A.out → B.in`, moving the `B.in` end to `C.in` produces
`A.out → C.in`.

The graph stays unchanged during the drag. Release in empty editor space
disconnects the chosen connection. Release on a compatible port moves the
chosen endpoint there. Right-clicking a port with several connections opens
a chooser whose entries can be dragged like individual port handles.

The suggested ambiguous-case rules remain under discussion. With exactly one
connection, Shift-drag can pick it immediately. With several connections,
Shift-click can open the same chooser as right-click. No arbitrary first
connection is selected. Release on the original port does nothing. Release
on an incompatible port or a grouped slot without choosing an individual
port cancels. Escape, pointer cancellation, or leaving the editor window
cancels rather than deleting a connection.

The implementation must resolve the chosen semantic connection and endpoint,
not operate on an aggregate drawn wire. Other connections and endpoints must
remain unchanged. Updating one connection can preserve its ID and use one
`graphConnections.upsert`. If an edit requires several record changes, they
must form one coordinated project action. Reattachment must not submit a
delete followed by a separate upsert and expose an intermediate disconnected
generation. The completed gesture is one future undo entry.


## Previous checkpoint

The owner next proposed Shift-drag disconnection/reattachment and a draggable
connection chooser on right-click. The proposal and suggested cancellation
rules are recorded in
[Proposed Disconnection And Endpoint Movement](./node_presentation_and_manual_controls_direction.md#proposed-disconnection-and-endpoint-movement).
The owner accepted the proposal on 2026-11-13 with one change: only Shift-click
opens the chooser for several connections. Right-click receives no new menu.
Implementation is in progress. The graph remains unchanged until release.
A simple endpoint move preserves its connection ID through one upsert; it
does not submit a deletion followed by a separate connection creation.


## Shift-click checkpoint before Shift-hover

Preserved on 2026-11-13 from the current gesture contract and implementation
checkpoint immediately before the owner requested Shift-hover instead of a
required opening click. The replacement is the linked current contract above.

A normal primary drag starts a new connection, including when the port already
has connections. Shift-drag moves an existing connection end. With exactly one
connection at that port, it picks up that end immediately. With several
connections, Shift-click opens a chooser. Each chooser entry is draggable with
the primary button. The editor must not pick an arbitrary connection.
Right-click receives no new menu.

With one connection, Shift-drag picks up that end; with several, Shift-click
opens a chooser with draggable entries. Right-click receives no new menu.

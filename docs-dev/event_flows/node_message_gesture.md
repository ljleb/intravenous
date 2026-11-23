# Event Flow: Node Message Gesture

A gesture groups history for several ordinary semantic node messages. It does
not keep a `BuilderSession`, application-module transaction, or mutex alive
between requests. The contract is in
[Node Interaction And Presentation Communication Direction](../node_interaction_and_presentation_communication_direction.md#accepted-constructor-values-and-grouped-message-gestures).

- `nodeMessages.beginGesture`: `SocketRpcServer -> ProjectGraph`. The server
  supplies the authenticated connection's client ID. `ProjectGraph` validates
  the target and records the open ID and address. There is no graph mutation.
- `nodeMessages.dispatch` with `interactionId`: uses the existing
  [node-message interaction tree](node_message_interaction.md). Each accepted
  replacement compiles and publishes normally. Its `ProjectEditEffect` joins the
  open group's earliest before-values and latest after-values.
- `nodeMessages.endGesture`: `SocketRpcServer -> ProjectGraph -> ProjectAutosave`
  when nonempty history is closed. `ProjectGraph` appends one history effect and
  removes the open group. An already closed group ends idempotently. This step
  changes no constructor data and compiles no graph.
- An overlapping accepted project edit closes the affected open groups inside
  `ProjectGraph`, before recording its own effect. It uses its existing event
  tree. Later dispatches for a closed group fail before invoking a node handler.
- Undo/redo close all open groups before applying the selected history effect.
  Disconnect closes that client's groups at their last accepted changes, within
  the existing disconnect cause and its one project-state notification.

Both begin/end and dispatch are authorized, revision-checked mutations. The
extension serializes each view-owned gesture and never retries a stale event.
View disposal closes accepted history even if begin was still pending.
Disconnect invalidates queued work so it cannot enter a replacement server.

`graph.getScope` returns accepted constructor arguments from retained
`ConfiguredNodeInstance` values. A curve editor reconciles its local point draft
with that read model. It uses the accepted payload's revision, not the later
history-only end revision, to know when a read includes the point update.
Neither this read nor semantic synchronization uses `Observations`.

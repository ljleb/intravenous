# Presentation execution and ownership

The presentation lives in a worker, while the editor owns the node, the lane,
and the observation subscriptions. This boundary lets the same guest run in
the browser and the headless client. Both workers import the same
[ABI adapter](../../../vscode/client/src/wasmPresentation.ts) and
[message endpoint](../../../vscode/client/src/wasmPresentationEndpoint.ts).

## One session, repeated frames

The host instantiates the module and calls `iv_create_session`. It restores
the saved state once before the first frame. A C++ presentation stores
working state in the class instance. For example, the marker keeps its drag
draft separate from the committed position so it can cancel a gesture.

On each frame the host writes dimensions, input, and observations and calls
`iv_present` once. An ordered gesture batch keeps button events and moves
in order when frames are coalesced. The guest writes drawing and targets
into a reusable output arena. The host validates those records before it
draws or routes input. `iv_reuse_drawing` saves geometry work for an
unchanged view. Hit targets and status can still change.

The host asks for JSON only at the persistence boundary. A replacement
worker can restore a lane's saved state and prove it can draw before it
takes over. The host postpones a swap while a gesture is active. An
invalid candidate leaves the active worker running.

## Why the host projects recorded triggers

The [Click Detector presentation](../../../src/intravenous/builtin_packages/builtin/presentations/click_detector.cpp)
reads a view of the node's recorded trigger output. Its manifest binds the
guest's `clicks` observation ID to the output port of the same name. The
server divides the requested time range into bounded bins. It finds each
bin's boundaries by binary search over the sorted recording and can count
coincident events exactly.

The editor keeps old ready windows while a newer request is pending. It
selects disjoint spans from those windows, clips bins to those spans, and
projects their first visible trigger to a display column. The guest receives
columns, a count, and a flag for a lower-bound count. A clipped bin can
prove that its first trigger is visible. Its total can include triggers
outside the clipped interval. The label therefore says “At least” when it
uses a clipped bin. This work belongs to the host because it already owns
the viewport, cache, and signed sample-index wrap. See the
[column projection](../../../vscode/client/src/presentationTriggerColumns.ts).

## Why the host projects the Beats grid

The [Beats presentation](../../../src/intravenous/builtin_packages/builtin/presentations/beats.cpp)
reads numeric constructor arguments and accepts input from its settings
controls. A preview updates the guest's draft and submits a complete
`SetBeatGrid` node event. The host uses the event identity and fingerprint to
validate it, groups successive previews into one undo gesture, and keeps
the final edit pending until accepted arguments arrive.
When project history interrupts editing, the host drops queued inputs and
discards any edit commands from the frame already running. The guest
receives a cancellation and clears its preview.

The guest declares a rational sample period, subdivision strides, and
drawing style. The host projects that grid at the current zoom and captures
the displayed grid when playhead seeking begins. During that drag, the
host calculates off-screen detents from the same captured declaration.
Both operations can continue while the worker is busy. Custom ruler grids
still get their marks through server observations because their event
positions can depend on arbitrary upstream nodes.

## How node edits return to the presentation

The guest records the edit during `iv_present` and can draw a draft in the
same frame. After the guest returns, the host checks the edit and queues
it through `PresentationLaneEdits`. The host uses the regular node-event
RPC to deliver it. The guest continues to receive viewport and input
updates while that RPC is pending. A reply reaches the original worker
as an `IvEditReply` in a subsequent frame. The accepted project snapshot
can arrive later. The request ID identifies the completed operation.
The accepted revision identifies the snapshot that includes it.
[Placement example](../tutorials/presentation_event.md)

Typed sample observations and exact event observations are separate from
node edits. See the
[migration notes](../../implementation_fronts_direction.md) for Automation
Curve's requirements.

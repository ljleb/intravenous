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

## What this port adds to the API

Click Detector needed typed trigger input, input connection status, lane
mode, and text output. It reuses the existing polyline batch for click
markers. The host continues to own playback, subscriptions, and theme
colors. The artifact carries its own icon and declares participation in
the host's timeline overlays.

The trigger view is useful for display. Consumers that need exact positions
or event payloads still need an event observation interface. The next
built-in ports also need typed beat and sample observations, seeking,
authoring inputs, edit replies, and cursor return. The Click Detector
does not need any of those functions. See the
[migration notes](../../implementation_fronts_direction.md) for the planned
ownership of those operations.

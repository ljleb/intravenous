# Builtin automation guide before exact sample editing

Historical excerpt from `src/intravenous/builtin_packages/builtin/AUTOMATION.md`
at revision `7a1e018f`, preserved on 2026-11-18. The current guide replaces
the seconds-only format and the original gestures.
[Current guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)

> Each point stores a unique ID, a position in seconds, a Sample value, an outgoing
> segment policy, and tension. Times are signed coordinates on the cyclic timeline.
> The source rounds seconds to sample indices and rejects indices outside
> `[-2^63, 2^63)`. Equal sample indices use ascending persistent point IDs.

> The Vue presentation creates an empty point list for a new curve. Double-click
> empty space to add a point. Drag a point to move it. Double-click a point to edit
> its outgoing segment and tension. Right-click a point to remove it. Point drags
> send coalesced `MovePoint` messages and form one undo operation per gesture.
> `AddPoint`, `RemovePoint`, `MovePoint`, and `SetSegment` replace the module's
> construction arguments. Project persistence and history retain their accepted
> values. The editor displays those construction values, including real gaps and
> the same automatic interpolation convention as the interpreter.

The first sample-editing pass on 2026-11-18 added this boundary behavior to
the same guide. The current guide uses the knob value scale without a drag
margin. [Current guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)

> Point dragging leaves a small
> margin at unbounded edges.

# Middle-button release before cursor placement at the wrapped position

_Historical, non-normative. Preserved on 2026-11-18 after the owner chose to
keep the cursor at the last displayed position on release._

Source and replacement are in
[Vue client implementation stages](../vue_client_implementation_stages.md#stage-2-implementation-status).

> Ctrl and Alt middle drags use pointer lock to draw a cursor that wraps within
> the timeline viewport. Release restores the system cursor to the press
> position. If the browser denies pointer lock, the drag uses pointer capture.

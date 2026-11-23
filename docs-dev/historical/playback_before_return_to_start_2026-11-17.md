# Playback before return to the selected start

Historical record preserved 2026-11-17 from the playback direction and its
implementation checkpoint. The owner requested paused startup at index zero
and return to the last selected start on pause. See the current
[transport contract](../project_playback_and_presentation_grids_direction.md#transport-and-one-continuous-foreground-execution-history).

The previous command table defined pause as follows.

| Action | Applied playhead | Next `ctx.index` |
| --- | --- | --- |
| Pause | Holds the selected position | Continues advancing on executed blocks |

The implementation checkpoint recorded that running was the initial state to
preserve the existing startup behavior. A running pause held the position
reached after the last successful Tick block. A resume started from that held
position. The client used ruler clicks to seek.

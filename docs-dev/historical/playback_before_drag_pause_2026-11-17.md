# Ruler dragging before temporary pause

Historical record preserved 2026-11-17 from the playback direction and its
implementation checkpoint. See the current
[transport contract](../project_playback_and_presentation_grids_direction.md#transport-and-one-continuous-foreground-execution-history).

The earlier direction said that the requested cursor followed the pointer
and the applied playhead followed backend snapshots. A drag kept the
transport in its current running or paused mode. The implementation sent
discrete seeks at the press, on animation frames during the drag, and at
release. The ruler kept its applied playhead marker visible during the drag.

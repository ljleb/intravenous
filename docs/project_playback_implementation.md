# Project playback implementation

Status: active implementation, 2026-11-17. The owner confirms the page-cache
tests pass and has run its benchmark. Playback is the next authorized feature.
Builds, tests, typechecks and profiling remain owner-run; Git staging belongs
to the owner.

The normative contract is
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).
Implement one coherent path: `GraphExecutor` applies ordered session commands
at Tick boundaries, distinguishes held project position from continued Tick
execution, prepares background windows, and supplies neutral/empty background
inputs while paused. Live Tick processing and history continue. RPC exposes
accepted commands and coherent applied snapshots; Vue draws the shared
playhead, displays samples/seconds, and provides controls independently of
viewport navigation. No graph rebuild or project undo entry follows a seek.

Implementation order:

1. Trace `GraphExecutor`, `SystemAudioDevices`, background preparation, session
   permissions/RPC ordering, and the timeline presentation. Record command and
   playback-window ownership before selecting the interfaces.
2. Implement ordered command application, coherent bounded snapshots, continuous
   Tick coordinates during pause, background-delivery suppression and resume/
   seek preparation through the existing executor boundary.
3. Wire typed session operations, JSON-RPC, extension commands and timeline
   rendering. Keep viewport position independent; never extrapolate across a
   seek or pause. Do not add tempo or graph-authored grids to this feature.
4. Add regressions after the interfaces take shape, then hand verification to
   the owner. Report unexpected semantic or ownership friction before changing
   the accepted contract.

No playback source change is claimed by this initial plan. The optional
node-facing callback playback structure follows a concrete node requirement;
it is not required for ordinary Tock clips or the initial playhead.

# Per-Realtime-Lane Meter Presentation State

## Goal

Remember the selected meter scale independently for every realtime sample
lane. The current choices are `decibel` and `sample`; unconfigured lanes
default to `decibel`.

The setting must be project state, rather than VS Code webview state, so it
survives a client reload and is shared by every client opening the project.

## Intended design

1. Store a meter-scale value against the lane's persistent public ID.
2. Expose that value in lane-view data so the client can render each meter
   independently.
3. Add an RPC/project command for changing one lane's scale from its meter
   context menu.
4. Persist the map as explicit `iv_project.jsonl` commands and restore it
   during project replay.
5. Treat newly created or unknown realtime lanes as dB meters until a setting
   exists.

## Constraints

- This is presentation state only; it must not alter audio execution, channel
  layout, or the realtime sample data.
- Values for lanes that do not currently exist must remain pending by public
  ID, just like pending public lane connections, so module realization order
  cannot discard them.
- A client should update the selected meter optimistically, then accept the
  project-backed value on subsequent lane-view refreshes.

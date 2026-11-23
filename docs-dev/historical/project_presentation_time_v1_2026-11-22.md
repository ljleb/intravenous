# Presentation time coordinates in version 1

_Historical record from 2026-11-22. Source: `PresentationSnapshot.viewport`
and `PresentationObservation.spans` in `vscode/client/src/presentationPlugin.ts`._

Version 1 used `{ beginSeconds, spanSeconds, sampleRate?, gridMode? }` for
`viewport.time`. Its manifest set `"version": 1`. Each observation span
contained `begin`, `end`, and `result`.

Version 2 uses an exact sample origin so that a viewport can cross the
signed sample seam at any zoom level. It also supplies a result window
offset for each observation span. See the
[current frame and input contract](../project_presentation_plugins.md#frame-and-input-contract).

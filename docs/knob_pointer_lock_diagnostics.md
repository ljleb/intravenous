# Knob pointer-lock recording

This client tool records the next knob drag. It does not change the graph or
the stored manual values beyond the knob gesture itself.

## Editor support

VSCodium 1.143.17232 uses VS Code 1.143.1, whose
[released source](https://github.com/microsoft/vscode/blob/1.143.1/src/vs/code/electron-main/app.ts#L253-L285)
contains the detached-window permission fix. The owner installed the fixed
release. Editor installation is managed outside this repository.

The temporary editor patches, package definitions, and test installation were
removed. This repository builds the Intravenous extension in `vscode/client`.
The investigation and previous procedures are preserved in the
[historical host-workaround record](historical/vscodium_pointer_lock_host_workaround_2026-11-14.md).

## Record a drag

1. Rebuild and install the client extension.
2. Open the Intravenous graph.
3. Open the Command Palette with Ctrl+Shift+P.
4. Run `Intravenous: Record Knob Pointer Lock`.
5. Drag a knob until the pointer escapes, then release the mouse.

The client saves a JSON report under `debug-release/` at the repository root
and opens it beside the graph. For a project outside Git, it saves under that
project's `debug-release/` directory. The repository ignores generated reports.
Send the JSON contents or the saved path for inspection.

Lock failures and webview boundary crossings stay in the recording until mouse
release. The recorder does not cancel the drag when acquisition fails. Native
focus loss also finishes the report after the normal focus handlers run.
If recording does not finish, run the same command again to stop it manually.
A successful drag also produces a report for comparison. The previous procedure
is [preserved here](historical/knob_pointer_recording_before_passive_failures_2026-11-14.md).

The report includes browser information, pointer-lock state, capture changes,
absolute and relative mouse coordinates, button state, focus changes, wheel
events, document attachment, and user activation. Browser rejections retain the
exception name as well as its message. It records the first events and a bounded tail, up to 5,000 events total.
`droppedEvents` counts omitted events from the middle of a long recording.
It does not record text-field contents or ordinary key values.

The recorder is idle until the command arms it. Arming waits for a knob drag,
so opening the Command Palette does not become the beginning of the recording.
The status bar reports when it is armed and when the drag starts.

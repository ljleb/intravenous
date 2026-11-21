# Knob pointer recording before passive failure capture

Historical implementation procedure, preserved on 2026-11-14 from
`docs/knob_pointer_lock_diagnostics.md`. The recorder originally stopped and
cancelled a drag on the first lock diagnostic. This hid focus recovery and
changed the gesture being measured. The replacement is
[Knob pointer-lock recording](../knob_pointer_lock_diagnostics.md).

# Knob pointer-lock recording

This client tool records the next knob drag. It does not change the graph or
the stored manual values beyond the knob gesture itself.

1. Rebuild and install the client extension.
2. Open the Intravenous graph.
3. Open the Command Palette with Ctrl+Shift+P.
4. Run `Intravenous: Record Knob Pointer Lock`.
5. Drag a knob until the pointer escapes, then release the mouse.

The client saves a JSON report under `debug-release/` at the repository root
and opens it beside the graph. For a project outside Git, it saves under that
project's `debug-release/` directory. The repository ignores generated reports.
Send the JSON contents or the saved path for inspection.

A detected lock failure, webview boundary crossing, or native focus loss also
finishes the report. If recording does not finish, run the same command again
to stop it manually. A successful drag also produces a report for comparison.

The report includes browser information, pointer-lock state, capture changes,
absolute and relative mouse coordinates, button state, focus changes, and wheel
events. It records the first events and a bounded tail, up to 5,000 events total.
`droppedEvents` counts omitted events from the middle of a long recording.
It does not record text-field contents or ordinary key values.

The recorder is idle until the command arms it. Arming waits for a knob drag,
so opening the Command Palette does not become the beginning of the recording.
The status bar reports when it is armed and when the drag starts.

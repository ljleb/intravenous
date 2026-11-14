# Removed knob pointer-lock recorder

Historical implementation record preserved on 2026-11-14 from
`docs/knob_pointer_lock_diagnostics.md` and `docs/vue_client_implementation_stages.md`.
The owner requested removal of the debugging Command Palette action after the
host permission issue was resolved. The recorder, protocol messages, file writer,
and recorder-specific tests were removed together. Pointer-lock behavior and
its automatic failure diagnostics remain. The editor compatibility warning also
remains. The current status is [here](../knob_pointer_lock_diagnostics.md).

## Previous diagnostic instructions

# Knob pointer-lock recording

This client tool records the next knob drag. It does not change the graph or
the stored manual values beyond the knob gesture itself.

## Editor support

VSCodium 1.143.17232 uses VS Code 1.143.1, whose
[released source](https://github.com/microsoft/vscode/blob/1.143.1/src/vs/code/electron-main/app.ts#L253-L285)
contains the detached-window permission fix. The owner installed the fixed
release. Editor installation is managed outside this repository.

The fix is already present in
[VS Code 1.133.0](https://github.com/microsoft/vscode/blob/1.133.0/src/vs/code/electron-main/app.ts#L246-L288).
When an Intravenous project activates in an older desktop editor, the extension
shows a warning and writes it to the Intravenous output channel. It uses
`vscode.version`, the core version shared by VS Code and VSCodium. The warning
suggests an update or keeping the graph in the main window. It does not block
the extension. Browser hosts do not receive this desktop-specific warning.

The temporary editor patches, package definitions, and test installation were
removed. This repository builds the Intravenous extension in `vscode/client`.
The investigation and previous procedures are preserved in the
[historical host-workaround record](vscodium_pointer_lock_host_workaround_2026-11-14.md).

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
is [preserved here](knob_pointer_recording_before_passive_failures_2026-11-14.md).

The report includes browser information, pointer-lock state, capture changes,
absolute and relative mouse coordinates, button state, focus changes, wheel
events, document attachment, and user activation. Browser rejections retain the
exception name as well as its message. It records the first events and a bounded tail, up to 5,000 events total.
`droppedEvents` counts omitted events from the middle of a long recording.
It does not record text-field contents or ordinary key values.

The recorder is idle until the command arms it. Arming waits for a knob drag,
so opening the Command Palette does not become the beginning of the recording.
The status bar reports when it is armed and when the drag starts.

## Previous implementation checkpoint

Inline knobs and an explicit pointer-lock recorder (2026-11-14):

- Directly visible eligible inputs now render their knob beside the wire handle.
  Port fitting uses each row's full height. Knob rows occupy more height than
  event/output handles. Folded ports keep their existing popovers, while direct
  input handles no longer open a redundant knob popover on ordinary hover.
- Input and output columns retain their separate positions. Narrow labels hide
  names/readouts before hiding the wire and knob targets. Connection-preview
  colors use the port-circle positions, not the width of the input column.
- The owner requested an explicit recorder after automatic anomaly messages
  remained absent. **Intravenous: Record Knob Pointer Lock** arms the next drag.
  It finishes on release or a detected failure. Running it again stops manually.
  Reports include the initial acquisition events and a bounded tail of recent
  events, even if the browser never reports the observed confinement failure.
- The extension saves reports to the repository's ignored `debug-release/`
  directory and opens the JSON beside the graph. This reintroduces a recording
  action at the owner's explicit request. It adds no buttons to the presentation.

Instructions are in [Knob Pointer-Lock Recording](../knob_pointer_lock_diagnostics.md).
Added regressions for mixed port-row heights, trace ownership, bounded recording,
and report-directory selection. Tests and client typechecks remain owner-run.
The reported `System Default [default]; active` status confirms hardware device
activation. It does not prove that the accepted graph delivers a nonzero signal.


# Knob pointer-lock diagnostics

The extension uses the standard browser pointer-lock API during knob drags.
The owner confirms that the host permission patch fixes detached-window locking
and now uses a normal fixed VSCodium release. Editor installation is managed
outside this repository.

The permission fix is present in
[VS Code 1.133.0](https://github.com/microsoft/vscode/blob/1.133.0/src/vs/code/electron-main/app.ts#L246-L288).
Project activation warns when an older desktop editor can reject pointer lock
in detached windows. The warning uses the VS Code core version, also exposed by
VSCodium. It offers an editor update or the main-window fallback and does not
block startup. Browser hosts do not receive the desktop-specific warning.

The owner requested removal of the recorder command on 2026-11-14. The command,
recorder, and JSON file writer were removed. Automatic acquisition-failure and
unexpected-unlock messages still appear in the Intravenous output channel.

The investigation and retired tools remain documented in the historical
[host-workaround record](historical/vscodium_pointer_lock_host_workaround_2026-11-14.md)
and [recorder-removal record](historical/knob_pointer_recorder_removed_2026-11-14.md).

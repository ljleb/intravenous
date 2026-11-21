# VSCodium pointer-lock investigation and retired host tooling

Historical implementation record preserved on 2026-11-14 from
`docs/knob_pointer_lock_diagnostics.md` and the final two editor-status sections
of `docs/vue_client_implementation_stages.md`.

The owner confirmed that the backport fixed detached-window pointer lock, then
installed the normal fixed release and moved its Nix package definition into
`/etc/nixos`. The owner requested removal of editor patches and installations
from this repository. The patch script, its tests, the optional editor package,
the generated package link, and the temporary test profile were removed.

The extension remains in `vscode/client`. Editor installation is managed outside
this repository. The current diagnostic procedure is
[Knob Pointer-Lock Recording](../knob_pointer_lock_diagnostics.md).
The commands below describe the retired investigation and are not current
installation instructions. References to removed tools are retained as text.

## Previous diagnostic and installation document

# Knob pointer-lock recording

This client tool records the next knob drag. It does not change the graph or
the stored manual values beyond the knob gesture itself.

## Current resolution: install a normal fixed release

VSCodium 1.143.17232 uses VS Code 1.143.1. The
[released VS Code source](https://github.com/microsoft/vscode/blob/1.143.1/src/vs/code/electron-main/app.ts#L253-L285)
contains the auxiliary-window permission checks. The upstream change is
[commit c073c561](https://github.com/microsoft/vscode/commit/c073c5616e5115e6fab18400610321a04f955081),
merged on 2026-08-05 under the title “Fix microphone permissions in auxiliary windows.”
Its core permission list also includes pointer lock.

The owner confirms that the local backport fixes pointer lock in detached windows.
A normal released editor is now the preferred installation. The earlier backport
and isolated-profile instructions below describe diagnosis of older versions.

`nix/vscodium-release.nix` (removed repository file: `nix/vscodium-release.nix`) selects the unmodified
VSCodium 1.143.17232 package from pinned nixpkgs revision
`d4f25a3e562008648d45bcf2d70bcc7422cad84e`. It does not change `flake.lock` or
the application's compiler environment. Its Nix syntax was parsed here. The
released package still needs an owner check in a detached window.

For this owner's system, replace the `vscodium` entry in
`environment.systemPackages` in `/etc/nixos/configuration.nix` with:

```nix
(import /home/ljleb/src/intravenous/nix/vscodium-release.nix {})
```

Then activate the configuration:

```bash
sudo nixos-rebuild switch
```

Fully close existing VSCodium processes once. Open `codium` normally from the
terminal or desktop launcher. Use the existing profile without `--user-data-dir`.
Both the repository and projects can use this editor. Test pointer lock after
detaching the graph. No extension rebuild or local JavaScript patch is involved.
The system configuration has not been edited by the agent.

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

## Observed host limitation, 2026-11-14

The first owner trace reports a DOM lock and fixed absolute coordinates throughout
the drag. The owner saw the cursor reappear outside the graph during that drag.
The second trace rejects acquisition with
`The root document of this element is not valid for pointer lock.`
That trace predates exception-name capture, so the message does not distinguish
a document error from a permission error.

Both reports identify VSCodium with Electron 39.8.7. Electron
[40.5.0](https://github.com/electron/electron/releases/tag/v40.5.0) includes a
[Wayland pointer-lock fix](https://github.com/electron/electron/pull/49283).
This is a candidate cause, not a confirmed diagnosis of the owner's display
backend. The user agent alone does not distinguish native Wayland from XWayland.

For a comparison, fully exit all VSCodium windows, then start the project with
`codium --ozone-platform=x11 --new-window /home/ljleb/src/intravenous/projects/simple_sine`.
The explicit X11 backend bypasses the native Wayland path. An existing VSCodium
process can absorb a launch and keep its previous backend, so a new window alone
is insufficient. Alternatively, use a VSCodium build whose Electron runtime
contains the linked fix. This repository does not select the editor's runtime.

The audio-output status `active` only confirms an active device. It does not
confirm an activated graph or nonzero output samples. The silent-project
investigation remains open.

## Default-backend and X11 comparison, 2026-11-14

The owner rebuilt the client and recorded two further drags. The second followed
a full VSCodium exit and an explicit X11 launch. Both drags visibly failed to lock.

| Report filename | Rejection after drag start | DOM lock acquired |
| --- | --- | --- |
| `knob-pointer-2026-11-14T15-16-47-929Z-dac0821b.json` | 0.9 ms | No |
| `knob-pointer-2026-11-14T15-18-52-294Z-14c62739.json` | 1.0 ms | No |

Both rejections are
`SecurityError: The root document of this element is not valid for pointer lock.`
Both traces report focus, user activation, and an attached surface in the current
document. Pointer capture remains available through release. These reports
identify a permission rejection before acquisition. The X11 comparison did not
resolve this rejection. It does not explain the earlier granted lock that failed
to confine the visible cursor.

Static inspection of the installed VSCodium 1.116.02821 bundle, commit
`221e0a382c0be3a673a4e4cab0601344a0b3de3a`, finds `pointerLock` in its allowlist.
Its default-session handler accepts requests only when `details.requestingUrl`
starts with `vscode-webview://` or the main editor's `vscode-file://` prefix.
Other URLs are rejected. Its webview iframe sandbox already contains
`allow-pointer-lock`. These are direct reads of the installed JavaScript, not
observations of its live permission callbacks.

Current [upstream VS Code](https://github.com/microsoft/vscode/blob/main/src/vs/code/electron-main/app.ts)
also accepts `about:blank` requests from registered auxiliary windows. The
installed bundle lacks that branch. Whether the owner uses a detached graph
window remains to be confirmed. A detached-window comparison is the next step.
The original Wayland candidate remains relevant only to the earlier granted-lock
trace until further evidence connects it to this failure.

## Detached-window result and optional host backport, 2026-11-14

The owner moved the graph into the main VSCodium window. Pointer lock then
worked without the observed escape. The remaining failure follows the detached
window. This supports the permission-handler diagnosis above. The host patch
still requires a manual check in a detached window.

The extension cannot override Electron's main-process permission decision.
The optional Nix package (removed repository file: `nix/vscodium-pointer-lock.nix`) applies a narrow
backport to VSCodium. It uses the repository's locked nixpkgs and does not
replace the system package. New repository files do not need Git staging for
this build. The expression imports only the patch script, not local build
artifacts.

Run these commands from the repository root:

```bash
nix-build nix/vscodium-pointer-lock.nix -o build-release/vscodium-pointer-lock
```

Launch the patched editor alongside the normal editor with a dedicated profile:

```bash
env -u NIXOS_OZONE_WL ./build-release/vscodium-pointer-lock/bin/codium \
  --ozone-platform=x11 \
  --user-data-dir "$PWD/debug-release/vscodium-pointer-lock-user" \
  --extensions-dir "$HOME/.vscode-oss/extensions" \
  --new-window "$PWD/projects/simple_sine"
```

The separate `--user-data-dir` prevents the normal editor process from absorbing
this launch. The normal editor can stay open. The explicit `--extensions-dir`
shares the installed VSCodium extensions, including the Intravenous client.
Settings and layout for this test editor remain separate from the normal profile.
The extension directory matches this installation's `.vscode-oss` data folder.

Keep X11 for this first comparison to isolate the permission change. This patch
does not backport Electron's separate native Wayland confinement fix.
Detach the graph tab and drag a knob. The test does not require another
extension build. After rebuilding the patched editor or changing its startup
flags, close only the editor that uses this dedicated profile before relaunching.
The normal editor can remain open throughout.

The [CLI documentation](https://code.visualstudio.com/docs/configure/command-line#_isolating-vs-code-instances)
describes this instance isolation. The previous shared-profile launch procedure
is [preserved here](patched_editor_before_isolated_profile_2026-11-14.md).

The patch script (removed repository file: `scripts/patch_vscodium_pointer_lock.py`) adds an exception
only when all these conditions hold:

- The requested permission is `pointerLock`.
- Electron identifies the request as a main-frame request for `about:blank`.
- VSCodium recognizes the requesting `webContents` in its auxiliary-window registry.

Both request and permission-check handlers use that condition. Other requests
retain the existing rules. This is narrower than the current upstream core-window
policy, which also handles other permissions. The script rejects unsupported
bundle shapes before writing either replacement. An editor that already includes
the upstream change also requires inspection instead of a blind replacement.

Validation completed here: the patch applies to a temporary copy of the installed
VSCodium bundle, Node accepts the resulting JavaScript syntax, the Nix expression
parses, and four focused patch tests pass. The tests execute both permission
handlers and cover rejected windows, frames, URLs, and unrelated permissions.
The editor package was not built or launched here.

To rerun the focused tests in an environment with Python and Node.js:

```bash
python3 scripts/test_patch_vscodium_pointer_lock.py
```

## Previous implementation-stage status

Detached-window knob pointer lock (2026-11-14):

- The owner confirms that pointer lock works with the graph in the main VSCodium
  window. The detached window still rejects acquisition with `SecurityError`.
- The installed editor permission handler lacks the upstream auxiliary-window
  branch. This is host friction, not a change to the intended knob interaction.
- `nix/vscodium-pointer-lock.nix` provides an optional editor backport. It allows
  `pointerLock` only for main-frame `about:blank` requests from registered
  auxiliary windows. The normal extension remains independent of this package.
- Static bundle syntax validation and four focused permission tests pass. The
  patched editor still needs owner validation in a detached window. Build and
  launch steps are in [Knob Pointer-Lock Recording](../knob_pointer_lock_diagnostics.md).

Fixed editor release identified (2026-11-14):

- The owner confirms that the local backport resolves detached-window pointer lock.
- Released VS Code 1.143.1 contains the upstream auxiliary-window permission fix.
  VSCodium 1.143.17232 uses that source and is packaged in current nixos-unstable.
- `nix/vscodium-release.nix` selects this normal release from a fixed nixpkgs
  revision. It applies no editor patches and leaves the application toolchain pin
  intact. This replaces the backport as the recommended installation path.
- The agent parsed the Nix expression but did not change the system configuration
  or activate an editor. Owner validation of the unmodified release remains.

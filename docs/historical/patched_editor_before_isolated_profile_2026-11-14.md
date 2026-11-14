# Patched editor launch before an isolated profile

Historical procedure preserved on 2026-11-14 from
`docs/knob_pointer_lock_diagnostics.md`. The shared profile required every
VSCodium window to close before testing a different binary. The owner requested
a way to keep the normal editor open. The replacement uses a separate
`--user-data-dir` and an explicit shared extension directory. See
[the current instructions](../knob_pointer_lock_diagnostics.md).

Fully quit all VSCodium windows. Then launch the patched package:

```bash
env -u NIXOS_OZONE_WL ./build-release/vscodium-pointer-lock/bin/codium \
  --ozone-platform=x11 --new-window "$PWD/projects/simple_sine"
```

Keep X11 for this first comparison to isolate the permission change. This patch
does not backport Electron's separate native Wayland confinement fix.

The installed extension and normal user profile remain available. Detach the
graph tab and drag a knob. The test does not require another extension build.
A previously running editor can absorb the launch, so closing all windows
beforehand remains necessary. To return to the system package, fully exit
the patched editor and launch the usual `codium` command.


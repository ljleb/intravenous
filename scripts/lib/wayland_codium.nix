{ codiumRoot }:

let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/4e160d1f018d84a4519a62f3630a95122f762c37.tar.gz";
    sha256 = "sha256-rhg4bi/56GfA7PdJ+Pp19qB+5TR0ZE9lQABdJybLOY4=";
  }) { };

  codium = builtins.storePath codiumRoot;
  electronSource = pkgs.electron-source.electron_43.unwrapped;
  electron = electronSource.overrideAttrs (old: {
    postPatch = old.postPatch + ''
      patch -p1 < ${../../docs-dev/upstream/chromium-150-wayland-pointer-position.patch}
    '';
  });
in
assert electron.version == "43.7.7";
assert electron.info.deps.src.args.tag == "150.0.7871.250";
pkgs.runCommand "intravenous-wayland-codium" {
  nativeBuildInputs = [ pkgs.makeWrapper ];
  passthru = { inherit electron; };
} ''
  runtime=${electron.dist}
  stock=${codium}/lib/vscode
  target="$out/lib/vscode"

  # An executable symlink would make Electron look for VSCodium's app next to
  # the original executable. Install the binary beside the editor resources.
  mkdir -p "$out/bin" "$target/resources"
  for path in "$runtime"/*; do
    case "''${path##*/}" in
      electron|resources) ;;
      *) ln -s "$path" "$target/" ;;
    esac
  done
  install -m755 "$runtime/electron" "$target/codium"
  ln -s "$stock/resources/app" "$stock/resources/completions" "$target/resources/"

  runtimeVersions='JSON.stringify([process.versions.electron, process.versions.chrome, process.versions.modules])'
  installed=$(ELECTRON_RUN_AS_NODE=1 "$stock/codium" -p "$runtimeVersions")
  patched=$(ELECTRON_RUN_AS_NODE=1 "$target/codium" -p "$runtimeVersions")
  if [ "$installed" != "$patched" ]; then
    echo "VSCodium expects $installed and the patched runtime provides $patched." >&2
    exit 1
  fi

  substitute ${codium}/bin/.codium-wrapped "$out/bin/.codium-cli" \
    --replace-fail "$stock" "$target"
  substitute ${codium}/bin/codium "$out/bin/.codium-env" \
    --replace-fail ${codium}/bin/.codium-wrapped "$out/bin/.codium-cli"
  chmod +x "$out/bin/.codium-cli" "$out/bin/.codium-env"

  # A separate profile makes the launch independent of the running editor.
  makeWrapper "$out/bin/.codium-env" "$out/bin/codium" \
    --set IV_WAYLAND_CURSOR_RETURN 1 \
    --add-flags '"--user-data-dir=''${XDG_CONFIG_HOME:-$HOME/.config}/intravenous-wayland-codium"'
''

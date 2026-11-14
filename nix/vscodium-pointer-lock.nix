# Optional host backport. This does not replace the system's VSCodium package.
# The default uses this repository's locked nixpkgs without copying the worktree
# into the Nix store, so local build artifacts and untracked files are irrelevant.
{ pkgs ? let
    locked = (builtins.fromJSON (builtins.readFile ../flake.lock)).nodes.nixpkgs.locked;
    source = builtins.fetchTree {
      inherit (locked) type owner repo rev narHash;
    };
  in import source.outPath { }
}:
pkgs.vscodium.overrideAttrs (previous: {
  postFixup = (previous.postFixup or "") + ''
    ${pkgs.python3}/bin/python3 ${../scripts/patch_vscodium_pointer_lock.py} \
      "$out/lib/vscode/resources/app/out/main.js"
    ${pkgs.nodejs}/bin/node --check "$out/lib/vscode/resources/app/out/main.js"
  '';
})

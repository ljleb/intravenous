{ nixpkgsPath, snapshotPath, clientPath, storePaths, projectDir }:

let
  pkgs = import nixpkgsPath { };
  snapshot = builtins.storePath snapshotPath;
  client = builtins.storePath clientPath;
  runtimeDependencies = map builtins.storePath
    (pkgs.lib.splitString "\n" storePaths);
  runtime = pkgs.runCommandLocal "intravenous-dev-runtime" {
    inherit snapshot runtimeDependencies;
    nativeBuildInputs = [ pkgs.cmake ];
  } ''
    mkdir -p "$out"
    ln -s "$snapshot/sdk" "$out/sdk"
    ln -s "$snapshot/runtime/"* "$snapshot/runtime/.intravenous_defaults" "$out/"
    cp -R "$snapshot/builtin_packages" "$out/"
    chmod -R u+w "$out/builtin_packages"
    touch "$out/.iv-dev-runtime"

    # Retain the captured compiler environment and ELF dependencies of the
    # binaries imported from the local CMake build.
    printf '%s\n' $runtimeDependencies > "$out/toolchain-store-paths"
    cmake "-Druntime=$out" "-Dscratch=$TMPDIR" \
      "-Dprebuild=$snapshot/iv_package_prebuild" -P "$snapshot/PrepareDevRuntime.cmake"
  '';
in
pkgs.runCommandLocal "intravenous-dev-bundle" {
  inherit client runtime;
  # VSCodium uses the extension path to identify its development windows.
  projectKey = builtins.hashString "sha256" projectDir;
} ''
  mkdir -p "$out/extension" "$out/bin"
  cp -R "$client/." "$out/extension/"
  chmod u+w "$out/extension"
  printf '%s\n' "$projectKey" > "$out/extension/.iv-dev-bundle"
  ln -s "$runtime" "$out/runtime"
  ln -s "$runtime/ivctl" "$out/bin/ivctl"
''

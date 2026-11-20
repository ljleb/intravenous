#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  fail "Usage: nix develop --command $0 PROJECT_DIRECTORY [BUILD_DIRECTORY]"
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
project_dir="$(cd "$1" && pwd -P)"
[[ -f "$project_dir/iv_project.jsonl" ]] || fail "No iv_project.jsonl in $project_dir"
build_dir="${2:-$repo_root/build-release}"
[[ -f "$build_dir/CMakeCache.txt" ]] || fail "Configure $build_dir with CMake before launching."
build_dir="$(cd "$build_dir" && pwd -P)"
server_dir="$build_dir/src/intravenous"
client_dir="$repo_root/vscode/client"
codium="$(command -v -- "${INTRAVENOUS_DEV_CODIUM:-codium}")" ||
  fail "No VSCodium executable found. Put your regular codium on PATH or set INTRAVENOUS_DEV_CODIUM."
[[ -n ${IV_DEV_NIXPKGS:-} ]] || fail "Run this script in the Intravenous Nix development shell."
[[ -x "$client_dir/node_modules/.bin/esbuild" ]] || fail "Run npm --prefix vscode/client ci in the development shell."

cmake --build "$build_dir" --target intravenous
[[ -f "$server_dir/PrepareDevRuntime.cmake" && -f "$server_dir/runtime-sdk/.snapshot" ]] ||
  fail "The intravenous build did not prepare the development runtime inputs in $server_dir."

require_linked_server() {
  local executable="$1" input
  [[ -x "$executable" ]] || fail "Missing server executable. Build the intravenous target in $build_dir."
  for input in "$server_dir/libiv_builder.so" "$server_dir/runtime-sdk/.snapshot" \
      "$server_dir/builtin_packages/builtin/"{iv_package.json,module.cpp}; do
    [[ -f "$input" ]] || fail "Missing $input. Build the intravenous target in $build_dir."
    [[ ! "$executable" -ot "$input" ]] ||
      fail "The server is older than $input. Finish building the intravenous target in $build_dir before launching."
  done
}

require_linked_server "$server_dir/intravenous"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/intravenous-dev.XXXXXXXX")"
trap 'rm -rf -- "$scratch"' EXIT
snapshot="$scratch/intravenous-dev"
client_snapshot="$scratch/intravenous-dev-client"
mkdir -p "$snapshot/runtime" "$snapshot/builtin_packages/builtin" "$client_snapshot"

for file in intravenous libiv_builder.so intravenous-client-helper iv-toolchain-environment; do
  [[ -f "$server_dir/$file" ]] || fail "Missing $server_dir/$file. Build the intravenous target first."
  cp -p "$server_dir/$file" "$snapshot/runtime/"
done
cp -p "$build_dir/tools/"{iv_clang_source_introspection_plugin.so,iv_package_finalize} "$snapshot/runtime/"
cp -p "$build_dir/tools/iv_package_prebuild" "$server_dir/PrepareDevRuntime.cmake" "$snapshot/"
cp -R "$server_dir/runtime-sdk" "$snapshot/sdk"
cp -p "$server_dir/builtin_packages/builtin/"{iv_package.json,module.cpp} "$snapshot/builtin_packages/builtin/"
require_linked_server "$snapshot/runtime/intravenous"
sed -e 's@^iv_package_pch=.*@iv_package_pch=iv_dsl.pch@' \
    -e 's@^iv_builtin_package_root=.*@iv_builtin_package_root=builtin_packages@' \
    -e 's@^iv_builtin_package_artifact_root=.*@iv_builtin_package_artifact_root=builtin_package_artifacts@' \
    "$server_dir/.intravenous_defaults" > "$snapshot/runtime/.intravenous_defaults"

cp -p "$client_dir/package.json" "$client_snapshot/"
IV_CLIENT_OUT_DIR="$client_snapshot/dist" npm --prefix "$client_dir" run build

# Adding a local snapshot does not register the store references embedded in
# its binaries or in the captured compiler configuration.
LC_ALL=C grep -aohE '/nix/store/[0-9a-z]{32}-[0-9A-Za-z+._?=-]+' \
  "$snapshot/runtime/"* "$snapshot/runtime/.intravenous_defaults" \
  "$snapshot/iv_package_prebuild" "$snapshot/PrepareDevRuntime.cmake" > "$scratch/store-paths" ||
  [[ $? -eq 1 ]]
store_paths="$(sort -u "$scratch/store-paths" | while IFS= read -r path; do
  if [[ -e "$path" ]]; then printf '%s\n' "$path"; fi
done)"
[[ -n "$store_paths" ]] || fail "Could not find the server's Nix toolchain. Build it in the development shell."
snapshot_store="$(nix-store --add "$snapshot")"
exec 7<"$snapshot_store/runtime/.intravenous_defaults"
client_store="$(nix-store --add "$client_snapshot")"
exec 6<"$client_store/package.json"
bundle="$(nix-build "$repo_root/scripts/dev_bundle.nix" --no-out-link \
  --argstr nixpkgsPath "$IV_DEV_NIXPKGS" --argstr snapshotPath "$snapshot_store" \
  --argstr clientPath "$client_store" --argstr storePaths "$store_paths" \
  --argstr projectDir "$project_dir")"
exec 8<"$bundle/extension/.iv-dev-bundle"

editor_args=()
if [[ ${INTRAVENOUS_DEV_XWAYLAND:-0} == 1 ]]; then
  editor_args+=(--ozone-platform=x11)
fi
printf 'Opening %s with %s\n' "$project_dir" "$bundle"
# Use the normal desktop CLI so VSCodium can handle the development launch.
env -u VSCODE_IPC_HOOK_CLI -u ELECTRON_RUN_AS_NODE "$codium" --new-window \
  --extensionDevelopmentPath="$bundle/extension" "${editor_args[@]}" "$project_dir"

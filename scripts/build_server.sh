#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ ${1:-} == --leased ]]; then
    shift
    build_dir="${IV_SERVER_BUILD_DIR:?Missing managed server build directory}"
    build_type="${IV_SERVER_BUILD_TYPE:?Missing managed build type}"
    if [[ ! -f "$build_dir/.iv-configured" || ! -f "$build_dir/build.ninja" ]]; then
        cmake -S "$repo_root" -B "$build_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE="$build_type" -DJUCE_DIR="${JUCE_DIR:-}"
        touch "$build_dir/.iv-configured"
    fi

    if (( $# )); then
        exec "$@"
    fi
    exec cmake --build "$build_dir" --target intravenous
fi

if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: scripts/build_server.sh [--build-type TYPE] [-- COMMAND...]'
    exit
fi

lease_options=()
while (( $# )) && [[ $1 != -- ]]; do
    lease_options+=("$1")
    shift
done
if (( $# )); then shift; fi
exec "${IV_DEV_CACHE:-iv-dev-cache}" server --source "$repo_root" "${lease_options[@]}" \
    -- "$repo_root/scripts/build_server.sh" --leased "$@"

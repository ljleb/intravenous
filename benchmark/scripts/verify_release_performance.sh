#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$repo_root/scripts/lib/use_managed_build.sh"
jobs="${IV_BUILD_JOBS:-16}"
benchmark_workspace="${IV_MODULE_BENCHMARK_WORKSPACE:-${TMPDIR:-/tmp}/intravenous-module-build-benchmark}"
module_path="benchmark/fixtures/playback/saw"

cd "$repo_root"

cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --parallel "$jobs"

"$build_dir/benchmark/iv_module_build_benchmark" \
    --module "$module_path" \
    --workspace "$benchmark_workspace" \
    --keep

"$build_dir/benchmark/iv_playback_benchmark"

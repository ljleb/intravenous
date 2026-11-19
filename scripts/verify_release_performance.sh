#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${IV_BUILD_JOBS:-16}"
benchmark_workspace="${IV_MODULE_BENCHMARK_WORKSPACE:-${TMPDIR:-/tmp}/intravenous-module-build-benchmark}"
module_path="benchmark/fixtures/playback/saw"

cd "$repo_root"

cmake --build build-release --parallel "$jobs"
ctest --test-dir build-release --parallel "$jobs"

build-release/benchmark/iv_module_build_benchmark \
    --module "$module_path" \
    --workspace "$benchmark_workspace" \
    --keep

build-release/benchmark/iv_playback_benchmark

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${IV_BUILD_JOBS:-16}"
benchmark_workspace="${IV_MODULE_BENCHMARK_WORKSPACE:-${TMPDIR:-/tmp}/intravenous-module-build-benchmark}"
module_path="projects/simple_sine/modules/saw"

cd "$repo_root"

cmake --build build-release --parallel "$jobs"
ctest --test-dir build-release --parallel "$jobs"

benchmark_marker="$(mktemp)"
trap 'rm -f "$benchmark_marker"' EXIT

for stage in \
    full; do
    build-release/benchmark/iv_module_build_benchmark \
        --module "$module_path" \
        --workspace "$benchmark_workspace" \
        --stage "$stage" \
        --gcc-time-report \
        --keep
done

built_modules=()
while IFS= read -r built_module; do
    built_modules+=("$built_module")
done < <(
    find "$benchmark_workspace/project/build/iv/build" \
        -type f \
        -path '*/Release/out/libiv_module_iv_project_saw.so' \
        -newer "$benchmark_marker" \
        -print
)

if (( ${#built_modules[@]} != 1 )); then
    printf 'Expected exactly one newly built saw module, found %d.\n' \
        "${#built_modules[@]}" >&2
    exit 1
fi

build-release/benchmark/iv_module_execution_benchmark \
    --module "${built_modules[0]}" \
    --warmup-blocks 4096 \
    --blocks 65536 \
    --blocks-per-sample 256

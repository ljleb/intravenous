#!/usr/bin/env bash

set -euo pipefail

shape="${1:-feedback}"
size="${2:-64}"
seconds="${3:-30}"
extra_benchmark_args=("${@:4}")
repo_root="$(git rev-parse --show-toplevel)"
build_dir="${IV_BUILD_DIR:-${repo_root}/build-release}"
benchmark="${build_dir}/benchmark/iv_edit_latency_benchmark"

if ! command -v perf >/dev/null 2>&1; then
    echo "perf is not available; enter the repository's default Nix development shell" >&2
    exit 1
fi
if [[ ! -x "${benchmark}" ]]; then
    echo "benchmark executable not found: ${benchmark}" >&2
    exit 1
fi
if [[ ! "${size}" =~ ^[1-9][0-9]*$ || ! "${seconds}" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [voices|chain|mixed|feedback|storage] [positive-size] [positive-seconds] [benchmark-options...]" >&2
    exit 1
fi

profile_dir="$(mktemp -d "${build_dir}/iv-perf-${shape}-${size}.XXXXXX")"
raw_data="${profile_dir}/perf.data"
jit_data="${profile_dir}/perf.jit.data"
report="${profile_dir}/report.txt"
# Everything the benchmark and perf print, including the steady Tick
# statistics and the edit timing breakdown.
benchmark_log="${profile_dir}/benchmark.log"
# The steady Tick statistics and the report's performance-core header.
summary="${profile_dir}/summary.txt"
# Always names the most recent profile of this shape and size.
latest="${build_dir}/iv-perf-${shape}-${size}.latest"

echo "Recording ${shape}, size ${size}, for ${seconds} steady-state seconds"
echo "Profile directory: ${profile_dir}"

JITDUMPDIR="${profile_dir}" IV_ORC_PERF_JITDUMP=1 \
    perf record -k 1 -F 999 -e cycles:u --call-graph dwarf \
        -o "${raw_data}" -- \
        "${benchmark}" \
            --shape "${shape}" \
            --size "${size}" \
            --profile-seconds "${seconds}" \
            "${extra_benchmark_args[@]}" \
    2>&1 | tee "${benchmark_log}"

if [[ -z "$(find "${profile_dir}" -type f -name 'jit-*.dump' -print -quit)" ]]; then
    echo "LLVM produced no ORC jitdump; the benchmark likely exited before profiling" >&2
    echo "Raw perf data remains at: ${raw_data}" >&2
    exit 1
fi

perf inject -j -i "${raw_data}" -o "${jit_data}"
perf report --stdio --no-children --comms iv_edit_latency \
    -i "${jit_data}" > "${report}"

{
    echo "shape ${shape}, size ${size}, ${seconds} steady-state seconds"
    grep -E '^steady Tick' "${benchmark_log}" || true
    grep -E -A1 "^# Samples: .* of event 'cpu_core/" "${report}" || true
} > "${summary}"
ln -sfn "$(basename "${profile_dir}")" "${latest}"

echo "Summary:            ${summary}"
echo "Benchmark output:   ${benchmark_log}"
echo "Symbolized samples: ${jit_data}"
echo "Text report:        ${report}"
echo "Latest profile:     ${latest}"
echo "Inspect interactively with: perf report --comms iv_edit_latency -i '${jit_data}'"
echo "Inspect one function with:  perf annotate -i '${jit_data}' --symbol FUNCTION"

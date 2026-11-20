#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: benchmark/scripts/benchmark_buffered_storage.sh MODE [ITERATIONS]

Modes
  buffered-storage  Measure overwrite and fragmentation costs.
  page-cache        Measure cache publication and reclamation with pinned readers.

ITERATIONS defaults to 100. Set IV_BUILD_DIR to use an existing build directory.
EOF
}

mode="${1:-}"
iterations="${2:-100}"
if [[ "$mode" == --help && $# == 1 ]]; then
    usage
    exit 0
fi
if [[ $# -gt 2 || ! "$iterations" =~ ^[1-9][0-9]*$ ||
      ( "$mode" != buffered-storage && "$mode" != page-cache ) ]]; then
    usage >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$repo_root/scripts/lib/use_managed_build.sh"
report_dir="$build_dir/$mode"
benchmark="$build_dir/benchmark/iv_buffered_storage_benchmark"

if [[ ! -x "$benchmark" ]]; then
    printf 'Build the iv_buffered_storage_benchmark target first: %s\n' "$benchmark" >&2
    exit 1
fi

run_case() {
    local report="$report_dir/$1"
    shift
    printf 'Writing %s\n' "$report"
    "$benchmark" "$@" 2>&1 | tee "$report"
}

mkdir -p "$report_dir"
for pages in 64 1024; do
    if [[ "$mode" == page-cache ]]; then
        for readers in 0 4; do
            run_case "pages-$pages-readers-$readers.tsv" --cache "$pages" "$iterations" "$readers"
        done
    else
        for pins in 0 4; do
            for mapping_kib in 64 1024; do
                run_case "pages-$pages-pins-$pins-view-$mapping_kib-KiB.tsv" \
                    "$pages" "$iterations" "$pins" "$mapping_kib"
            done
        done
    fi
done

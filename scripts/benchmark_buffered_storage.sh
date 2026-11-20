#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/use_managed_build.sh"
iterations="${1:-100}"
report_dir="$build_dir/buffered-storage"
benchmark="$build_dir/benchmark/iv_buffered_storage_benchmark"

if [[ $# -gt 1 || ! "$iterations" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' 'Usage: scripts/benchmark_buffered_storage.sh [iterations=100]' >&2
    exit 2
fi
if [[ ! -x "$benchmark" ]]; then
    printf 'Build the iv_buffered_storage_benchmark target first: %s\n' "$benchmark" >&2
    exit 1
fi
mkdir -p "$report_dir"
for pages in 64 1024; do
    for pins in 0 4; do
        for mapping_kib in 64 1024; do
            report="$report_dir/pages-$pages-pins-$pins-view-$mapping_kib-KiB.tsv"
            printf 'Writing %s\n' "$report"
            "$benchmark" "$pages" "$iterations" "$pins" "$mapping_kib" 2>&1 | tee "$report"
        done
    done
done

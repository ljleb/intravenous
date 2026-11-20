#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/use_managed_build.sh"
iterations="${1:-100}"
benchmark="$build_dir/benchmark/iv_buffered_storage_benchmark"
report_dir="$build_dir/page-cache"
if [[ $# -gt 1 || ! "$iterations" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' 'Usage: bash scripts/benchmark_page_cache.sh [iterations=100]' >&2
    exit 2
fi
if [[ ! -x "$benchmark" ]]; then
    printf 'Build the iv_buffered_storage_benchmark target first: %s\n' "$benchmark" >&2
    exit 1
fi
mkdir -p "$report_dir"
for pages in 64 1024; do
    for readers in 0 4; do
        report="$report_dir/pages-$pages-readers-$readers.tsv"
        printf 'Writing %s\n' "$report"
        "$benchmark" --cache "$pages" "$iterations" "$readers" 2>&1 | tee "$report"
    done
done

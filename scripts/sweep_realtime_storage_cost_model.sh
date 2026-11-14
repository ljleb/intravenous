#!/usr/bin/env bash

set -euo pipefail

mode="${1:-quick}"
size="${2:-64}"
iterations="${3:-3}"
settle_ms="${4:-500}"

repo_root="$(git rev-parse --show-toplevel)"
build_dir="${IV_BUILD_DIR:-${repo_root}/build-release}"
benchmark="${build_dir}/benchmark/iv_edit_latency_benchmark"

if [[ ! -x "${benchmark}" ]]; then
    echo "benchmark executable not found: ${benchmark}" >&2
    exit 1
fi
if [[ ! "${size}" =~ ^[1-9][0-9]*$ \
    || ! "${iterations}" =~ ^[1-9][0-9]*$ \
    || ! "${settle_ms}" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 [quick|coarse] [positive-size] [positive-iterations] [positive-settle-ms]" >&2
    exit 1
fi
if [[ "${mode}" != "quick" && "${mode}" != "coarse" ]]; then
    echo "unknown sweep mode: ${mode}" >&2
    exit 1
fi

output_dir="$(mktemp -d "${build_dir}/iv-storage-cost-sweep.XXXXXX")"
all_results="${output_dir}/results.tsv"
metadata="${output_dir}/metadata.txt"
latest="${build_dir}/iv-storage-cost-sweep.latest"
workspace="${output_dir}/workspace"
summarizer="${repo_root}/scripts/summarize_realtime_storage_cost_sweep.py"

echo "Sweep directory: ${output_dir}"

# label|stack budget or default|copied|ring-addressed|stack-footprint|persistent-footprint
profiles=(
    "default|default|1|0|0|2"
    "copy-4|default|4|0|0|2"
    "ring-2|default|1|2|0|2"
    "stack-2|default|1|0|2|2"
    "persistent-0|default|1|0|0|0"
    "stack-budget-256k|262144|1|0|0|2"
)
if [[ "${mode}" == "coarse" ]]; then
    profiles+=(
        "copy-0|default|0|0|0|2"
        "copy-2|default|2|0|0|2"
        "ring-1|default|1|1|0|2"
        "ring-4|default|1|4|0|2"
        "stack-1|default|1|0|1|2"
        "stack-4|default|1|0|4|2"
        "persistent-1|default|1|0|0|1"
        "persistent-4|default|1|0|0|4"
        "balanced|default|2|1|1|2"
        "stack-budget-64k|65536|1|0|0|2"
        "stack-budget-1m|1048576|1|0|0|2"
    )
fi

# label|shape|size|block size or default
cases=(
    "voices|voices|${size}|default"
    "chain|chain|${size}|default"
    "mixed|mixed|${size}|default"
    "feedback|feedback|${size}|default"
    "storage|storage|${size}|default"
    "storage-large-block|storage|${size}|2048"
)

{
    echo "git_commit=$(git -C "${repo_root}" rev-parse HEAD)"
    echo "mode=${mode}"
    echo "size=${size}"
    echo "iterations=${iterations}"
    echo "settle_ms=${settle_ms}"
    echo "benchmark=${benchmark}"
} > "${metadata}"

first_result=1
run_count=0
total_runs=$((${#profiles[@]} * ${#cases[@]}))
for profile in "${profiles[@]}"; do
    IFS='|' read -r profile_label stack_budget copied ring stack persistent \
        <<< "${profile}"
    for benchmark_case in "${cases[@]}"; do
        IFS='|' read -r case_label shape case_size block_size \
            <<< "${benchmark_case}"
        run_count=$((run_count + 1))
        stem="${profile_label}--${case_label}"
        run_tsv="${output_dir}/${stem}.tsv"
        run_log="${output_dir}/${stem}.log"
        command=(
            "${benchmark}"
            --shape "${shape}"
            --size "${case_size}"
            --iterations "${iterations}"
            --settle-ms "${settle_ms}"
            --copied-byte-weight "${copied}"
            --ring-addressed-byte-weight "${ring}"
            --stack-footprint-byte-weight "${stack}"
            --persistent-footprint-byte-weight "${persistent}"
            --results-tsv "${run_tsv}"
            --workspace "${workspace}"
        )
        if [[ "${stack_budget}" != "default" ]]; then
            command+=(--stack-budget-bytes "${stack_budget}")
        fi
        if [[ "${block_size}" != "default" ]]; then
            command+=(--block-size "${block_size}")
        fi

        echo "[${run_count}/${total_runs}] ${profile_label}: ${case_label}"
        "${command[@]}" 2>&1 | tee "${run_log}"

        if ((first_result)); then
            {
                printf 'profile\tcase\t'
                head -n 1 "${run_tsv}"
            } > "${all_results}"
            first_result=0
        fi
        while IFS= read -r row; do
            printf '%s\t%s\t%s\n' "${profile_label}" "${case_label}" "${row}"
        done < <(tail -n +2 "${run_tsv}") >> "${all_results}"
    done
done

python3 "${summarizer}" "${all_results}" "${output_dir}"
ln -sfn "$(basename "${output_dir}")" "${latest}"
echo "Sweep complete"
echo "Results:  ${all_results}"
echo "Tick/edit summary: ${output_dir}/tick-edit-summary.tsv"
echo "Plan summary:      ${output_dir}/plan-summary.tsv"
echo "Metadata: ${metadata}"
echo "Logs:     ${output_dir}/*.log"
echo "Latest:   ${latest}"

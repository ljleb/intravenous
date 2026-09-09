#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${IV_BUILD_DIR:-$repo_root/build-release}"
jobs="${IV_BUILD_JOBS:-16}"
workspace="${IV_MODULE_BENCHMARK_WORKSPACE:-${TMPDIR:-/tmp}/intravenous-module-reload-profile}"
module_paths=()
optimization="O3"
skip_build=0
simple_sine_modules=0
verbose=0

usage() {
    cat <<'EOF'
Usage: scripts/profile_module_reload.sh [options]

Measures a cold IV-module build followed by a source-only hot reload in an
isolated retained workspace.

Options:
  --module PATH        Module directory or iv_module.json to snapshot.
                       May be specified more than once.
  --simple-sine-modules
                       Profile every module in projects/simple_sine/modules.
  --workspace PATH     Managed benchmark workspace.
  --optimization O0|O3 Finalizer optimization level (default: O3).
  --skip-build         Reuse an already-built iv_module_build_benchmark.
  --verbose            Print the full CMake/Ninja and compiler transcript.
  --help               Show this help.

Environment:
  IV_BUILD_DIR         Build directory (default: <repo>/build-release).
  IV_BUILD_JOBS        Parallel build jobs (default: 16).
  IV_MODULE_BENCHMARK_WORKSPACE
                       Default workspace when --workspace is omitted.
EOF
}

while (($#)); do
    case "$1" in
    --module)
        (($# >= 2)) || { printf '%s\n' '--module requires a path' >&2; exit 2; }
        module_paths+=("$2")
        shift 2
        ;;
    --simple-sine-modules)
        simple_sine_modules=1
        shift
        ;;
    --workspace)
        (($# >= 2)) || { printf '%s\n' '--workspace requires a path' >&2; exit 2; }
        workspace="$2"
        shift 2
        ;;
    --optimization)
        (($# >= 2)) || { printf '%s\n' '--optimization requires O0 or O3' >&2; exit 2; }
        optimization="$2"
        shift 2
        ;;
    --skip-build)
        skip_build=1
        shift
        ;;
    --verbose)
        verbose=1
        shift
        ;;
    --help)
        usage
        exit 0
        ;;
    *)
        printf 'Unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if (( simple_sine_modules )); then
    for manifest in "$repo_root"/projects/simple_sine/modules/*/iv_module.json; do
        [[ -f "$manifest" ]] || continue
        module_paths+=("${manifest#"$repo_root"/}")
    done
fi

if (( ${#module_paths[@]} == 0 )); then
    module_paths=("projects/simple_sine/modules/saw")
fi

case "$optimization" in
O0|O3) ;;
*)
    printf '%s\n' '--optimization must be O0 or O3' >&2
    exit 2
    ;;
esac

cd "$repo_root"

log_directory="${workspace}.logs"
mkdir -p "$log_directory"

print_phase_summary() {
    local module_name="$1"
    local phase="$2"
    local log_file="$3"

    awk -v module="$module_name" -v phase="$phase" '
        $1 == "iv-module-build-benchmark" && $2 == "phase=" phase {
            if ($3 ~ /^workload=/) {
                for (field_index = 1; field_index <= NF; ++field_index) {
                    split($field_index, field, "=")
                    values[field[1]] = field[2]
                }
            } else if ($3 ~ /^finalizer_/) {
                split($3, field, "=")
                stages[field[1]] = field[2]
            }
        }
        END {
            if (!("pipeline_ms" in values)) exit 1
            printf "iv-module-build-profile module=%s phase=%s pipeline_ms=%s export_ms=%s link_ms=%s finalizer_ms=%.1f jit_ms=%.1f runtime_o3_ms=%.1f object_emit_ms=%.1f native_link_ms=%.1f\n", \
                module, phase, values["pipeline_ms"], values["export_ms"], values["link_ms"], \
                stages["finalizer_total_us"] / 1000, \
                stages["finalizer_jit_materialize_us"] / 1000, \
                stages["finalizer_runtime_optimize_us"] / 1000, \
                stages["finalizer_native_object_emit_us"] / 1000, \
                stages["finalizer_native_link_us"] / 1000
        }
    ' "$log_file"
}

if (( ! skip_build )); then
    build_log="$log_directory/driver-build.log"
    if ! cmake --build "$build_dir" --target iv_module_build_benchmark --parallel "$jobs" \
        >"$build_log" 2>&1; then
        cat "$build_log" >&2
        exit 1
    fi
    if (( verbose )); then
        cat "$build_log"
    fi
fi

for module_index in "${!module_paths[@]}"; do
    module_path="${module_paths[module_index]}"
    module_name="${module_path%/}"
    if [[ "${module_name##*/}" == "iv_module.json" ]]; then
        module_name="${module_name%/*}"
    fi
    module_name="${module_name##*/}"

    if (( ${#module_paths[@]} == 1 )); then
        module_workspace="$workspace"
    else
        module_workspace="$workspace/$((module_index + 1))-$module_name"
    fi

    run_log="$log_directory/$((module_index + 1))-$module_name.log"
    if ! "$build_dir/benchmark/iv_module_build_benchmark" \
        --module "$module_path" \
        --workspace "$module_workspace" \
        --optimization "$optimization" \
        --keep >"$run_log" 2>&1; then
        printf 'IV-module benchmark failed for %s; full log follows: %s\n' \
            "$module_path" "$run_log" >&2
        cat "$run_log" >&2
        exit 1
    fi

    if ! print_phase_summary "$module_name" cold "$run_log" \
        || ! print_phase_summary "$module_name" hot "$run_log"; then
        printf 'Could not summarize IV-module benchmark output for %s; full log follows: %s\n' \
            "$module_path" "$run_log" >&2
        cat "$run_log" >&2
        exit 1
    fi
    if (( verbose )); then
        cat "$run_log"
    fi
done

printf 'Retained profiling workspace root: %s\n' "$workspace"
printf 'Detailed benchmark logs: %s\n' "$log_directory"

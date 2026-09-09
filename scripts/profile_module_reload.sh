#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${IV_BUILD_DIR:-$repo_root/build-release}"
jobs="${IV_BUILD_JOBS:-16}"
workspace="${IV_MODULE_BENCHMARK_WORKSPACE:-${TMPDIR:-/tmp}/intravenous-module-reload-profile}"
module_path="projects/simple_sine/modules/saw"
optimization="O3"
skip_build=0

usage() {
    cat <<'EOF'
Usage: scripts/profile_module_reload.sh [options]

Measures a cold IV-module build followed by a source-only hot reload in an
isolated retained workspace.

Options:
  --module PATH        Module directory or iv_module.json to snapshot.
  --workspace PATH     Managed benchmark workspace.
  --optimization O0|O3 Finalizer optimization level (default: O3).
  --skip-build         Reuse an already-built iv_module_build_benchmark.
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
        module_path="$2"
        shift 2
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

case "$optimization" in
O0|O3) ;;
*)
    printf '%s\n' '--optimization must be O0 or O3' >&2
    exit 2
    ;;
esac

cd "$repo_root"

if (( ! skip_build )); then
    cmake --build "$build_dir" --target iv_module_build_benchmark --parallel "$jobs"
fi

"$build_dir/benchmark/iv_module_build_benchmark" \
    --module "$module_path" \
    --workspace "$workspace" \
    --optimization "$optimization" \
    --keep

printf 'Retained profiling workspace: %s\n' "$workspace"
printf '%s\n' 'The finalizer sidecar is below its module cmake-build directory:'
find "$workspace" -type f -name iv-module-finalizer-timings.txt -print

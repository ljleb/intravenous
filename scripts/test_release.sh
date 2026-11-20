#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/test_release.sh [-j<n> | --help]

Build the preconfigured build-release tree and run CTest with
--output-on-failure. Hide the start and result lines for passing tests.

Options
  -j<n>                Use n parallel jobs for both the build and tests.
  -h, --help            Show this help and exit.

Environment
  IV_BUILD_JOBS        Default job count when -j is omitted (default 16).
EOF
}

if (( $# > 1 )); then
    usage >&2
    exit 2
fi

jobs="${IV_BUILD_JOBS:-16}"
case "${1-}" in
    "") ;;
    -h|--help)
        usage
        exit 0
        ;;
    -j*)
        jobs="${1#-j}"
        ;;
    *)
        printf 'Unexpected argument %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    printf 'Expected a positive integer for the job count, got "%s".\n' "$jobs" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build-release"

cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --parallel "$jobs" --output-on-failure 2>&1 |
    awk '
        /^[[:space:]]*Start[[:space:]]+[0-9]+:[[:space:]]/ {
            test_id = $2 + 0
            start[test_id] = $0
            name[test_id] = $0
            sub(/^[^:]+:[[:space:]]*/, "", name[test_id])
            next
        }
        /^[[:space:]]*[0-9]+\/[0-9]+[[:space:]]+Test[[:space:]]+#[[:space:]]*[0-9]+:/ {
            result = $0
            sub(/^[^#]+#[[:space:]]*/, "", result)
            test_id = result + 0
            sub(/^[0-9]+:[[:space:]]*/, "", result)

            # Wait for the result before deciding whether to show its Start line.
            if (test_id in start && index(result, name[test_id]) == 1) {
                status = substr(result, length(name[test_id]) + 1)
                passed = status ~ /^[.[:space:]]+Passed[[:space:]]+[0-9.]+[[:space:]]+sec[[:space:]]*$/
                if (!passed)
                    print start[test_id]
                delete start[test_id]
                delete name[test_id]
                if (passed)
                    next
            }
        }
        { print; fflush() }
        END {
            for (test_id in start)
                print start[test_id]
        }
    '

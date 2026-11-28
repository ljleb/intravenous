#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/test_release.sh [-j<n> | --help]

Build the preconfigured build-release tree and run CTest and the
TypeScript client tests. Passing tests update one live line in a terminal.

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
client_dir="$repo_root/vscode/client"

if ! command -v node >/dev/null || ! command -v npm >/dev/null; then
    printf '%s\n' 'Node.js and npm are required. Run this script in the development shell.' >&2
    exit 1
fi
if [[ ! -x "$client_dir/node_modules/.bin/tsc" ]]; then
    printf 'Install the TypeScript test dependencies with npm --prefix "%s" ci.\n' "$client_dir" >&2
    exit 1
fi

cmake --build "$build_dir" --parallel "$jobs"

terminal_columns=0
if [[ -t 1 && "${TERM:-dumb}" != dumb ]]; then
    { terminal_size="$(stty size <&3 2>/dev/null)" || terminal_size=; } 3>&1
    terminal_columns="${terminal_size##* }"
fi

filter_test_output() {
    awk -v columns="$terminal_columns" -v runner="$1" '
        function clear_progress() {
            if (showing_progress) {
                printf "\r\033[K"
                showing_progress = 0
            }
        }
        function show_progress(line) {
            if (columns < 5)
                return
            if (length(line) >= columns)
                line = substr(line, 1, columns - 4) "..."
            printf "\r\033[K%s", line
            fflush()
            showing_progress = 1
        }
        runner == "node" && /^[[:space:]]*✔ / && / \([0-9.eE+-]+ms\)[[:space:]]*$/ {
            show_progress($0)
            next
        }
        runner == "ctest" && /^[[:space:]]*Start[[:space:]]+[0-9]+:[[:space:]]/ {
            test_id = $2 + 0
            start[test_id] = $0
            name[test_id] = $0
            sub(/^[^:]+:[[:space:]]*/, "", name[test_id])
            next
        }
        runner == "ctest" && /^[[:space:]]*[0-9]+\/[0-9]+[[:space:]]+Test[[:space:]]+#[[:space:]]*[0-9]+:/ {
            result = $0
            sub(/^[^#]+#[[:space:]]*/, "", result)
            test_id = result + 0
            sub(/^[0-9]+:[[:space:]]*/, "", result)

            # Wait for the result before deciding whether to show its Start line.
            if (test_id in start && index(result, name[test_id]) == 1) {
                status = substr(result, length(name[test_id]) + 1)
                passed = status ~ /^[.[:space:]]+Passed[[:space:]]+[0-9.]+[[:space:]]+sec[[:space:]]*$/
                start_line = start[test_id]
                delete start[test_id]
                delete name[test_id]
                if (passed) {
                    show_progress($0)
                    next
                }
                clear_progress()
                print start_line
            }
        }
        { clear_progress(); print; fflush() }
        END {
            clear_progress()
            for (test_id in start)
                print start[test_id]
        }
    '
}

ctest --test-dir "$build_dir" --parallel "$jobs" --output-on-failure 2>&1 | filter_test_output ctest

npm --prefix "$client_dir" run test:prepare
(
    cd "$client_dir"
    FORCE_COLOR=0 node --test --test-concurrency="$jobs" --test-reporter=spec "dist-test/test/**/*.test.js"
) 2>&1 | filter_test_output node

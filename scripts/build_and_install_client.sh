#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$repo_root"
cmake --build build --target intravenous iv_module_shared iv_clang_source_span_rewriter --parallel 8
./scripts/install_client.sh "$repo_root/build/src/intravenous"

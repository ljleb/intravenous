#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$repo_root"
cmake --build build --target intravenous iv_module_shared iv_clang_source_introspection_plugin iv_module_finalize --parallel 16
./scripts/install_client.sh "$repo_root/build/src/intravenous"

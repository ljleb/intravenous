#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$repo_root"
cmake --build build-release --target intravenous iv_module_shared iv_gcc_source_introspection_plugin_build --parallel 16
./scripts/install_client.sh "$PWD/build-release/src/intravenous"

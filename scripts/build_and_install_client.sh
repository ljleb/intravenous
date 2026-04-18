#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$repo_root"
nix-shell --run "cmake --build build --target intravenous --parallel && ./scripts/install_client.sh"

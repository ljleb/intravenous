#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
client_dir="$repo_root/vscode/client"
vsix_path="$client_dir/intravenous-client.vsix"
backend_dir="${1:-}"

if [[ -n "$backend_dir" && "$backend_dir" != /* ]]; then
  echo "backend directory must be absolute: $backend_dir" >&2
  exit 2
fi

cd "$client_dir"
rm -f "$vsix_path"
INTRAVENOUS_DEFAULT_DIR="$backend_dir" npm run build
vsce package --allow-missing-repository --skip-license --out "$vsix_path"
codium --install-extension "$vsix_path" --force

echo "installed Intravenous client from $vsix_path"

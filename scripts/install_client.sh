#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
client_dir="$repo_root/vscode/client"
vsix_path="$client_dir/intravenous-client.vsix"

cd "$client_dir"
rm -f "$vsix_path"
npm run build
vsce package --allow-missing-repository --skip-license --out "$vsix_path"
codium --install-extension "$vsix_path" --force

echo "installed Intravenous client from $vsix_path"

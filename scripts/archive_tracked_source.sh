
#!/usr/bin/env bash
set -euo pipefail

# Usage: repo-zip.sh [repository-directory] [output.zip]

repo="${1:-.}"

# Resolve repository root
root="$(git -C "$repo" rev-parse --show-toplevel)"
root="$(cd "$root" && pwd -P)"
name="$(basename "$root")"

# Default output is alongside the repository
output="${2:-$(dirname "$root")/$name.zip}"

# Resolve output to an absolute path
mkdir -p "$(dirname "$output")"
output="$(cd "$(dirname "$output")" && pwd -P)/$(basename "$output")"

# Require a normal clone with a .git directory
if [[ ! -d "$root/.git" ]]; then
    echo "Error: Expected a .git directory (not a worktree or submodule)." >&2
    exit 1
fi

command -v zip >/dev/null || {
    echo "Error: 'zip' is required." >&2
    exit 1
}

# Create a temporary archive
tmp="$(mktemp "$(dirname "$output")/.repo-zip.XXXXXXXX.zip")"
trap 'rm -f -- "$tmp"' EXIT

# Archive committed files using GitHub-style rules
git -C "$root" archive \
    --format=zip \
    --prefix="$name/" \
    HEAD \
    -o "$tmp"

# Add the complete .git directory, preserving symlinks
(
    cd "$(dirname "$root")"
    zip -q -r -y "$tmp" "$name/.git"
)

# Replace output without leaving stale ZIP entries
mv -f -- "$tmp" "$output"

echo "Created: $output"

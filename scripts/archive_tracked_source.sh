#!/usr/bin/env bash
set -euo pipefail

# Usage: archive_tracked_source.sh [repository-directory] [output.zip]

repo="${1:-.}"
root="$(git -C "$repo" rev-parse --show-toplevel)"
root="$(cd "$root" && pwd -P)"
name="$(basename "$root")"
parent="$(dirname "$root")"

# The archive sits beside the repository by default. This keeps it out of the
# tracked working tree that it contains.
output="${2:-$parent/$name.zip}"
mkdir -p "$(dirname "$output")"
output="$(cd "$(dirname "$output")" && pwd -P)/$(basename "$output")"

# This archive intentionally contains the complete Git repository. A worktree
# or submodule has a .git file with different ownership rules, so reject it.
if [[ ! -d "$root/.git" ]]; then
    echo "Error: expected a .git directory, not a worktree or submodule." >&2
    exit 1
fi

if [[ "$output" == "$root/.git" || "$output" == "$root/.git/"* ]]; then
    echo "Error: output cannot be inside the .git directory it archives." >&2
    exit 1
fi

command -v zip >/dev/null || {
    echo "Error: 'zip' is required." >&2
    exit 1
}

tmp="$(mktemp "$(dirname "$output")/.archive-tracked-source.XXXXXXXX.zip")"
rm -f -- "$tmp"
trap 'rm -f -- "$tmp"' EXIT

# Add the current working-tree contents of every tracked path. Use bounded
# batches so a repository with many paths cannot exceed the command-line size
# limit. The exact output path is excluded if the caller places it in the tree.
tracked_paths=()
append_tracked_paths()
{
    if ((${#tracked_paths[@]} == 0)); then
        return
    fi
    (
        cd "$parent"
        zip -q -y "$tmp" "${tracked_paths[@]}"
    )
    tracked_paths=()
}

while IFS= read -r -d '' path; do
    source="$root/$path"
    if [[ "$source" == "$output" ]]; then
        continue
    fi
    if [[ ! -e "$source" && ! -L "$source" ]]; then
        echo "Error: tracked file is missing from the working tree: $path" >&2
        exit 1
    fi
    tracked_paths+=("$name/$path")
    if ((${#tracked_paths[@]} == 256)); then
        append_tracked_paths
    fi
done < <(git -C "$root" ls-files -z --cached)
append_tracked_paths

# Add repository history and local Git metadata after the working tree.
(
    cd "$parent"
    zip -q -r -y "$tmp" "$name/.git"
)

mv -f -- "$tmp" "$output"
trap - EXIT
echo "Created: $output"

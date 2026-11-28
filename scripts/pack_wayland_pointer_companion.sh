#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$repo/vscode/client/gnome-shell/cursor-return"
output_dir="$(realpath -- "${1:-.}")"
stage="$(mktemp -d)"
trap 'rm -rf -- "$stage"' EXIT

cp -- "$source_dir"/{metadata.json,extension.js,pointerCapture.js} "$stage/"
# Mutter 50's Clutter typelib omits the direction annotations on the
# outputs. Describe the existing symbol without modifying Mutter.
g-ir-compiler --includedir "$source_dir/bindings" \
    --output "$stage/IvClutter-1.0.typelib" "$source_dir/bindings/IvClutter-1.0.gir"
gnome-extensions pack --force --out-dir "$output_dir" \
    --extra-source=pointerCapture.js --extra-source=IvClutter-1.0.typelib "$stage"

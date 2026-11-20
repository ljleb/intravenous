# Sourced by benchmark drivers after selecting the repository root.
if [[ -n ${IV_BUILD_DIR:-} ]]; then
    build_dir="$IV_BUILD_DIR"
elif [[ -n ${IV_SERVER_BUILD_DIR:-} ]]; then
    build_dir="$IV_SERVER_BUILD_DIR"
else
    exec python3 "$repo_root/scripts/build_cache.py" server -- bash "$repo_root/scripts/${0##*/}" "$@"
fi

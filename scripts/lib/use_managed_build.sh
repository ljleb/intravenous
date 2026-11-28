# Sourced by benchmark drivers after selecting the repository root.
if [[ -n ${IV_BUILD_DIR:-} ]]; then
    build_dir="$IV_BUILD_DIR"
elif [[ -n ${IV_SERVER_BUILD_DIR:-} ]]; then
    build_dir="$IV_SERVER_BUILD_DIR"
else
    script_path="$(realpath -e -- "$0")"
    exec "$repo_root/scripts/build_server.sh" -- bash "$script_path" "$@"
fi

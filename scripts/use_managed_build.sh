# Sourced by benchmark drivers after selecting the repository root.
if [[ -n ${IV_BUILD_DIR:-} ]]; then
    build_dir="$IV_BUILD_DIR"
elif [[ -n ${IV_SERVER_BUILD_DIR:-} ]]; then
    build_dir="$IV_SERVER_BUILD_DIR"
else
    exec "$repo_root/scripts/build_server.sh" -- bash "$repo_root/scripts/${0##*/}" "$@"
fi

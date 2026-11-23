# Separate server and client installation

Archived on 2026-11-19 from `docs-dev/server_installation.md` and
`docs-dev/vue_ui_tuning.md`. One installer now publishes the extension and its
server together. See the [combined installer](./managed_install_before_nix_dev_run_2026-11-19.md)
and the [development workflow](../development_run.md).

The previous workflow used three commands:

```sh
cmake --build build-release --target intravenous --parallel 16
bash scripts/install_server.sh build-release
./scripts/install_client.sh "$PWD/.intravenous-install/build-release/current"
```

> The first command builds the server. The second command prepares and publishes
> one installed release. Run the third command when the extension changes or when
> you first switch it to the installed server. The VS Code build task runs only
> the first command. The installer also accepts a separate installation directory
> as its second argument.

The visual tuning guide used the third command to install client changes.

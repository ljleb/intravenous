# Client installation before server releases

Archived on 2026-11-19 from `docs/vue_ui_tuning.md`. That command installed
the extension with a default pointing directly into the server build tree.
See the [development workflow](../development_run.md).

> After editing, run this from the repository root to rebuild and install just the
> extension, then reopen the graph view:

```sh
./scripts/install_client.sh "$PWD/build-release/src/intravenous"
```

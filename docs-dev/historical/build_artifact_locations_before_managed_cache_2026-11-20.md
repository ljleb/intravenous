# Build artifact locations before the managed cache

These excerpts from revision `969442d3` record the old checkout-local server
build and the old artifact locations for project and common packages. The
[managed cache](../development_run.md#configure-the-build-cache) replaces
those defaults. The [package pipeline architecture](../package_pipeline_architecture.md#package-roots-and-build-artifact-ownership)
records the new package ownership rules.

## Development run

From `docs-dev/development_run.md`:

> From the repository root, configure the server build in the Nix development
> shell. Install the client build dependencies once, or again when its lockfile
> changes.

```sh
nix develop
cmake -S . -B build-release -G Ninja -DJUCE_DIR="$JUCE_DIR" -DCMAKE_BUILD_TYPE=Release
npm --prefix vscode/client ci
```

> To build the server on
> its own, run `cmake --build build-release --target intravenous`.

> Writable package caches stay in the project's
> build directory.

## Package artifact routes

From `docs-dev/package_pipeline_architecture.md`:

| Provenance | Artifact root |
| --- | --- |
| `built_in` | The explicit executable-owned `artifact_root`; omission is an error |
| `common` | `IV_GLOBAL_MODULE_CACHE`, or its platform default |
| `project_local` | The containing IV project's `build/iv/` directory |

> Every project opened by a compatible executable should reuse the same built-in
> package artifacts. Opening a project must not create a built-in build workspace
> under the built-in source directory or under that project's `build/iv/`.

# Development Launch Before Local Build Reuse

This workflow appeared in [Run Intravenous for development](../development_run.md).
On 2026-11-20, the owner asked the launcher to reuse the local build used by
the release test script.

> Inside the development shell, run `./scripts/run_dev.sh projects/simple_sine`
> directly. To use a CMake tree outside the managed cache, pass its directory
> as the second argument. You can also use the **Run Intravenous Development** editor task.
>
> The launcher configures a Release tree in the build cache on first use. It
> runs the incremental `intravenous` build before it builds the extension.

The client build wrote directly into a new scratch directory on each launch.
The [current workflow](../development_run.md) copies both build outputs from
the local CMake tree.

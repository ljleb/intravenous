# Front 11 Pre-Implementation Checkpoint, 2026-11-02

_Status: historical. This file preserves the package-artifact implementation
boundary that was replaced when Front 11 landed. Current requirements and current
implementation are documented in
[Package Pipeline Application Architecture](../package_pipeline_architecture.md)._

Before Front 11, `IvPackageDeclaration` carried only `package_id` and
`package_root`. `PackageWatcherService` passed path-only search roots to package
discovery. `ModuleLoader` inferred whether a package was shared by testing whether
its source path was under one of its constructor search roots.

The normative package-pipeline document described the implementation boundary as
follows:

> **Current implementation boundary:** CMake stages the built-in manifest and
> entry source under its binary tree and exposes that directory through
> `iv_package_search_root`. `ModuleLoader` currently treats all configured extra
> search roots as global and places their build workspaces under
> `IV_GLOBAL_MODULE_CACHE` (or its platform default); project-local workspaces
> use `<project>/build/iv/`. Dedicated executable-owned built-in artifacts,
> explicit three-scope provenance, and build/install-time prebuilding are future
> package-pipeline work. This section does not change the current loader or the
> ongoing `GraphJit` implementation plan.

Front 11 replaced that path-derived ownership with explicit
`PackageProvenance`, `PackageDiscoveryRoot`, and `PackageBuildRequest` records.
CMake now prebuilds the staged built-in package into an executable-owned artifact
root, and package compilation uses provenance-specific artifact ownership.

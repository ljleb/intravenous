# Package Bitcode O0 Contract

_Historical status: non-normative. This file preserves the statements that the
finalized package bitcode consumed by the project JIT is Clang O0 LLVM, as they stood
before the accepted change of 2026-11-07._

_Sources:
[Graph JIT Direction](../graph_jit_direction.md) (the "Current implementation
checkpoint" section and the definition-world section) and
[Port Realization Migration Status](../port_realization_migration_status.md) (the
active-feature description of node-facing port access). Preserved 2026-11-07._

_Reason for replacement: every `GraphJit` compilation re-optimized the imported
package code from Clang O0 form, which is work that no graph edit changes. Front 16
measured this at 64 voices with an in-process profiling knob that ran LLVM's ThinLTO
pre-link pipeline once per package revision. The per-edit optimization phase fell from
about 3.6 s to about 1.95 s, and the realtime tick did not get slower (about 1,500 µs
against 1,535 µs). The human co-author then set the principle that anything an edit
cannot invalidate must already be computed before a JIT compilation is launched.
Package translation units are now compiled to optimizer-ready Clang LLVM (`-O2 -Xclang
-disable-llvm-passes`), and `iv_package_finalize` runs the pre-link pipeline once per
package build. O0 output also carried no TBAA metadata and no lifetime markers, which
cannot be recovered after compilation._

_Replacement: [Graph JIT Direction](../graph_jit_direction.md) ("Current
implementation checkpoint" and the definition-world section);
[Port Realization Migration Status](../port_realization_migration_status.md);
measurements under Front 16 in
[implementation_fronts_direction.md](../implementation_fronts_direction.md)._

## Preserved text

From `graph_jit_direction.md`, "Current implementation checkpoint":

> `GraphJit` now captures one exact `ConfiguredGraph`/`NodeDefinitionsSnapshot`
> generation, resolves registered concrete nodes to the accepted package revisions
> that created them, parses those revisions' finalized O0 bitcode, indexes compiler
> callbacks by `NodeCodeKey`, …

From `graph_jit_direction.md`, the definition-world section:

> Each accepted `PackageRevision` pins both its native callbacks/lifetime and the
> finalized O0 bitcode used by the project JIT.

From `port_realization_migration_status.md`:

> The generated binding is initialized from immutable realization bytes, selected
> package callbacks are imported into the project LLVM module, and the whole module
> is optimized after package O0/no-inline barriers are removed. The selected value
> type is therefore available as a realization constant for inlining and dead-code
> elimination rather than requiring per-value variant dispatch.

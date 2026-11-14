# GraphJit Whole-Project Inlining Requirement

_Historical status: non-normative. This file preserves the requirement that every
project compilation imports the primitive callback closures, so that whole-project
inlining and optimization remain possible, as it stood before the accepted change of
2026-11-07._

_Source: [Graph JIT Direction](../graph_jit_direction.md), the section on the two
JITs (package JIT and project JIT). Preserved 2026-11-07._

_Reason for replacement: Front 16 measured the cost at 64 voices. Inlining the imported
`__iv_graph_primitive_*` operations into the root cost about 2.5 s per edit (3.1 s
against 0.61 s) and gave no measurable tick benefit (about 1,170 µs against about
1,160 µs). The root builds each instance's `ReflectedNodeTickContext` at runtime, so
the inlined copies had nothing constant to specialize. `import_primitive_callback()`
only renames a package callback and promotes its linkage. Primitive code therefore
depends only on the package revision. The human co-author's principle, that
anything a graph edit cannot invalidate must already be computed before a
compilation is launched, places that code outside the per-edit compilation. The
human co-author also observed that whole-project inlining is not necessarily
beneficial: its value depends on how the tick scheduling and batch decisions
([Batched Node Callback Direction](../batched_node_callbacks_direction.md) §8, which
the human co-author calls "tick packing") arrange the work. Some batch callbacks
prefer sample-major iteration over many lanes; others prefer one lane's block at a
time. The human co-author accepted the change "for now" on 2026-11-07, with the goal
of speeding up without sacrificing generality._

_Replacement: [Graph JIT Direction](../graph_jit_direction.md), the same section;
measurements under Front 16 in
[implementation_fronts_direction.md](../implementation_fronts_direction.md)._

## Preserved text

> The native DSP output of the first JIT is **not** the preferred code-generation
> input to the second JIT. `GraphJit` imports the required callback closures from
> the retained primitive node LLVM associated with the exact providers used by the
> configured graph so whole-project inlining and optimization remain possible. The
> accepted native node declaration/lifecycle callbacks remain useful for the canonical
> `NodeLayout`/`NodeStorage` contract and must stay pinned by the exact accepted
> package revisions.

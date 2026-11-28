# Generated Realtime Root

_Historical status: non-normative. This file preserves the design text under which
every project compilation generated the realtime root as LLVM: the root's binding
records were LLVM-visible constants, and `CompiledGraph` carried the generated
root's `tick_block` symbol._

_Source: [Graph JIT Direction](../graph_jit_direction.md), section "Tick/Sequential
port realization rules": the bullet "Bindings are immutable compiler facts" and the
paragraph after the rule list. Preserved 2026-11-10._

_Reason for replacement: Front 16 measured the per-edit compilation at size 64
(2026-11-10). Optimization and code generation took about 95% of GraphJit time for
the `voices`, `mixed`, and `feedback` shapes, and only `chain` met the 200 ms
edit-to-activation target. The human co-author set the next dependency-ordered
work on 2026-11-10: replace the per-edit generated LLVM root with precompiled root
execution that consumes immutable graph-dependent data, keeping graph-specific
compilation only for measured, profitable fused operations cached by content
(Front 16, "Remaining implementation order", item 2). The section on the two JITs
already allowed "the root, or data that a precompiled root consumes" as the
graph-dependent output of a per-edit compilation._

_Replacement: [Graph JIT Direction](../graph_jit_direction.md), the same section.
The realtime root is application code, `graph_jit::execute_realtime_root()`, which
executes one immutable `graph_jit::RealtimeRootPlan`. Implementation status is under
Front 16 in [implementation_fronts_direction.md](../implementation_fronts_direction.md)._

## Graph JIT Direction: "Bindings are immutable compiler facts"

```text
- **Bindings are immutable compiler facts.** Port implementation kind, offsets,
  capacities, layouts, history/latency parameters, and branch relationships should
  be emitted as immutable LLVM-visible records whenever possible. The generated
  root resolves each binding to its already-selected stack or `NodeStorage`
  pointer before invoking the node; fetching a port must not branch between
  storage classes. Only concrete base pointers and the current sample index/block
  size are dynamic values.
```

## Graph JIT Direction: paragraph after the realization rules

```text
The shell continues to use the generated-root and canonical realtime
`NodeLayout`/`NodeStorage` contract specified in this document: `CompiledGraph`
carries the finalized `NodeLayout` plus the generated root `tick_block`. Realtime
state lifecycle stays in `NodeStorage`; background-only lifecycle such as
`TockState` is owned separately by `BackgroundExecutor`. Primitive `skip_block` callbacks
are internal scheduling tools, not root operations. Whole-project lowering must not
reintroduce a second **realtime** node-storage layout or a synthetic project-wide
coverage/Tock ABI merely to expose internal Tock outputs.
```

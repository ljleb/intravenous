# Front 16 Region-Specialization Plan Before The First Successful Profile

_Status: non-normative historical material._

This note preserves the Front 16 optimization order and performance conclusion
that the first successful region-specialization profile superseded on
2026-11-11. The current status and order are in
[Implementation Fronts Direction](../implementation_fronts_direction.md).

The earlier active order was:

1. Keep the completed mutually-ready cyclic-region batching.
2. Move a compact sample carry whose endpoints belong to one cyclic
   `ExecutionRegionPlan` from its steps to that complete region.
3. Profile the carry change.
4. Replace `initial_callback_batch_lane_limit` with a planned-storage-budget
   lane count.
5. Add content-addressed `ExecutionRegionPlan` specialization.
6. Extend immutable-value propagation through specialized regions.

The profile before region specialization attributed the low-pass callback,
`tanf()`, and `powf()` costs to generic package callback execution. The status
text therefore said that region specialization plus immutable-value
propagation would remove both this authored work and the generic per-sample
port access.

The first successful specialized profile separated those effects. It showed a
large improvement in Tick time. However, `tanf()` and `powf()` remained in the
specialized function. A constant storage binding lets LLVM remove ordinary
loads and wrappers. It does not make strict floating-point library calls
compile-time evaluable. Planner-level immutable-value propagation remains
separate work. It now precedes the lane count from the storage budget.


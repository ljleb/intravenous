# Historical Linear And Saturating Global Index Semantics

_Status: non-normative historical material. Preserved 2026-10-13 from the global
timeline semantics previously spread across `ports.h`,
`coverage_and_background_evaluation.md`, and
`sequential_port_storage_planning.md`. It was superseded by the accepted circular
timeline design in [Circular Global Timeline Indexing](../global_indexing.md)._

The previous source represented the coordinate as:

```cpp
using SampleIndex = std::uint64_t;
using EventTime = size_t;
```

It treated that unsigned value as a linear timeline starting at zero. Addition used a
`saturating_sample_index_add()` helper which returned `UINT64_MAX` on overflow, and
history windows clamped their beginning to zero.

The previous normative Tick-event window was:

```text
[max(0, B - H), B + N + L)
```

The previous positional range was a begin/end pair:

```cpp
struct IndexRegion {
    SampleIndex begin;
    SampleIndex end;
};
```

It was valid only when `begin <= end`, contained `x` when
`begin <= x && x < end`, and could not express a range crossing the unsigned wrap
point. The old `Coverage` helper sorted and coalesced these regions by raw unsigned
global index.

Separately, earlier value-type documents used the name `GlobalIndex` for a planned
Q64.64 continuous port value: a transported fractional coordinate with the full
integer width plus 64 fractional bits. The accepted terminology reserves
`GlobalIndex` for the fundamental integer coordinate and renames that transported
value `FractionalGlobalIndex`.

These choices are preserved here because they explain zero-clamping, saturation,
begin/end region pairs, page-number arithmetic, and raw timestamp comparisons that may
remain visible while the source migration is incomplete. They are not compatibility
requirements.


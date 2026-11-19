# Sparse Tock before reverse transport and Timeline observation

This checkpoint came from the Shared storage prerequisites for sparse Tock
section of the Vue implementation stages on 2026-11-19.
[Current implementation status](../vue_client_implementation_stages.md#shared-storage-prerequisites-for-sparse-tock)
replaces it.

Reverse propagation still carries sample demand in `IndexCoverage`.
`StaticOutputCoverageRequirement::required()` returns a single range for a
Region output and asserts if that demand has several ranges. Compact
`required_positions()` must replace that assumption in the authored
facades, reflected records, and propagation workspace. The runtime still
needs to carry snapshot-scoped `RegionRef`s into prepared runs.

Cross-window sample reuse and indexed event observation remain pending.
Indexed event observation also requires exact occurrence counts and
index-specific completeness.

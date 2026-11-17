# Background page retention and reclamation

Status: active implementation, 2026-11-17. The owner confirms that the output
gain feature passes tests and works in the application. This is the next
complete feature. Playhead work follows owner verification of this feature.

## Accepted contract

The normative contract is in
[output capabilities](./output_capabilities_and_background_demand.md#reconstruction-and-cache-priority-owner-decision-2026-11-17).
Computed materializations favor reclamation unless expected reuse justifies
retention. Region/Coverage buffered contents favor retention within resource
budgets and can be reconstructed for the same semantic snapshot.
Sequential/buffered contents are authoritative recordings: their only
recoverable copy cannot be discarded. Reader pins and prepared execution
requirements prevent reclamation. Reclaiming payload never fabricates absence,
changes logical Regions or substitutes a newer input version.

## Implementation order

1. Trace the existing shared page allocator, immutable versions, mappings,
   validity metadata and transaction preparation. Record reconstruction inputs
   and ownership constraints before choosing eviction boundaries.
2. Implement page retention classification, admission and bounded background
   reclamation through the shared storage owner. Preserve recording bytes and
   pinned versions. Keep mapping, payload validity and logical coverage separate.
3. Integrate reconstruction and cache reuse into demand-scoped transactions,
   graph revision reuse, observation reads and playback preparation. No
   allocation, eviction, mapping or reconstruction enters the audio callback.
4. Add semantic and resource-pressure regressions after the implementation
   interfaces are established. Provide owner-run measurements if required.
5. Ask the owner to rebuild and run tests. Fix reported failures before
   starting the playhead feature.

No implementation change or test result is claimed by this plan. The owner
runs builds, tests, typechecks and profiling. The agent does not manipulate
Git staging or commits. Unexpected reconstruction, memory or dependency costs
must be reported before adding a workaround or changing the contract.

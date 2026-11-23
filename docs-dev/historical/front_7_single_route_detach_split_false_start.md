# Front 7 single-route detach split false start

This note preserves analysis that appeared in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
[`runtime_performance_roadmap.md`](../runtime_performance_roadmap.md) on 2026-10-21.
The replacement is in the Front 7 sections of those files.

The superseded analysis treated a final event or continuous route as having one
optional detach. It proposed two conversion records for a detached route: one fused
mapping into detach storage and one fused mapping from that storage to the concrete
consumer. It also said that a structural connection chain with a detach needed this
split to preserve the authored position of the detach relative to conversions.

The analysis correctly rejected two serial detaches on one dependency route. It also
correctly rejected a persistent general route-stage graph. Its error was to give the
detach an exact position inside storage and conversion lowering. `detach()` marks a
dependency that GraphJit removes when it forms the execution DAG. Stateless sample
transforms and event conversions can move across that dependency cut when source
ordering, target ordering, and the requested delay remain unchanged.

The old text also assumed that `SampleConnectionPlan` and `EventConnectionPlan` could
store at most one detach for the complete connection. That representation loses
information when only some sample channels or event sources are detached, or when
different sources use different detach latencies. The replacement stores detach
settings on each `ConfiguredSampleConnectionSource` and
`ConfiguredEventConnectionSource`, then carries them into the corresponding GraphJit
source records. Storage planning can still use one shared feedback buffer when every
source has the same detach settings.

The superseded next step was:

> Add one explicit detach split to sample and event connection plans. Record the
> detach storage type, the fused mapping into it, and the fused mapping out of it.

That step is no longer part of Front 7. The implementation instead preserves the
dependency setting per source and lets storage planning select the efficient physical
realization.

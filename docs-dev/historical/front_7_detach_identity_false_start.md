# Front 7 detach identity false start

This note preserves analysis that appeared in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
[`runtime_performance_roadmap.md`](../runtime_performance_roadmap.md) on 2026-10-21.
The analysis assigned an identity and a storage operation to each `detach()` call.
That model was incorrect. The replacement is in the Front 7 sections of those files.

The superseded analysis said that `ConfiguredGraph` lost detach identity.
It treated one detached stereo expression as different from two detached mono
expressions with equal settings. It also treated reuse of one detached expression as
different from separate `detach()` calls with equal settings.

The analysis proposed an explicit detach handle in `GraphBuilderState` and
`ConfiguredGraph`. The handle would have identified shared detach storage. It would
also have preserved the sample or event expression at the call to `detach()`.

This proposal gave `detach()` a storage meaning that it does not have. `detach()`
marks graph dependencies that GraphJit removes to break a cycle. Storage layout,
storage sharing, and materialization are later planning decisions. Equal detach
settings on equal source contributions have equal meaning. They do not require an
authored operation identity.

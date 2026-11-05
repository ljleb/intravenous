# Front 4 status before closure, 2026-10-23

_Status: historical, non-normative. Commit `641276f4` ("front 4 completion")
replaced the Front 4 implementation-status section of
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md)
without retaining it. This file restores the replaced text, as it stood at commit
`636edab1`, according to the documentation mutation protocol. The current status is
in [`implementation_fronts_direction.md`](../implementation_fronts_direction.md)._

The replaced section read:

> **Implementation status (2026-10-23):** background planning, storage,
> transactions, materialized playback, neutral fill, and generated-root
> `TickInvocationCall` binding exist. Realtime planning now accepts background-only
> Sequential sample and event inputs. `PrimitiveSampleInputBindingPlan` and
> `PrimitiveEventInputBindingPlan` mark those inputs as `invocation_supplied`, so
> they do not acquire false realtime storage. Disconnected Sequential event inputs
> share compiler-emitted immutable empty sequences.
>
> Mixed Tick/background composition, disconnected Random Access inputs, and
> cross-kind graph-revision history transfer remain incomplete. An
> `EventConnectionPlan` retains its original `targets`, `target_plans`, and
> per-delivery `EventDeliveryPlan::mechanism`; realtime lowering must consume the
> realtime subset without cloning the connection record.
>
> Cross-kind transition work needs an explicit interface between
> `TickInvocationWorkspace` and transition preparation. The predecessor playback
> window is not stored in predecessor `NodeStorage`, which is the only source that
> `RealtimePortStateTransitionPlan` can currently address. Until that interface is
> implemented, changing a Sequential input between background and realtime delivery
> is an explicit compilation error. The required semantics and the superseded
> handoff steps are retained in
> [Front 4 Opus handoff](./front_4_opus_handoff_2026-10-23.md).
>
> **Can start now:** yes. Prefer consuming the target Region/Coverage representation
> from front 3 when practical, but do not merge the ownership of the two fronts.

Two statements in it are design guidance rather than status, and they remain useful:

- realtime lowering consumes the realtime subset of an `EventConnectionPlan` without
  cloning the connection record; per-delivery `EventDeliveryPlan::mechanism` is the
  representation it reads; and
- Front 4 should consume Front 3's Region/Coverage representation where practical
  without merging ownership of the two fronts.

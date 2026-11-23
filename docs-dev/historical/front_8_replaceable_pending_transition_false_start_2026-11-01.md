# Front 8 Replaceable Pending Transition False Start

_Historical status: superseded on 2026-11-01 by the ordered accepted-transition
contract in [Project Graph Application Architecture](../project_graph_application_architecture.md),
[Realtime / Background Execution, Queues, And Capacity](../realtime_background_execution_and_queues.md),
and [Implementation Fronts Direction](../implementation_fronts_direction.md)._

## Provenance

The text below was active in `implementation_fronts_direction.md`,
`runtime_performance_roadmap.md`, `realtime_background_execution_and_queues.md`,
`sequential_port_storage_planning.md`, and `graph_jit_direction.md`. It also described
the temporary `GraphExecutor::pending_generation_`, `GraphExecutor::prepare_stage()`,
and `GraphExecutorStageResult::active_predecessor_changed` implementation.

## Superseded design

The earlier design treated the latest compiled generation as one replaceable pending
candidate. `GraphExecutor::prepare_stage()` captured the active `CompiledGraph` and its
`CompiledGraphRealtimePhase`. `GraphJit` specialized a successor from that snapshot.
`GraphExecutor::stage()` then rejected the result with
`active_predecessor_changed` if realtime had activated a different graph or if the
captured transition phase had expired. The application coordinator was expected to
compile the transition again. A newer pending generation could replace an older
pending generation before either became active.

The associated runtime text said:

> A staged generation is not active merely because preparation completed. A pending
> successor that never activates may be superseded and reclaimed off realtime without
> creating a semantic generation boundary.

The associated transition text said:

> If another graph edit supersedes the transition before its expiry, the next rebuild
> uses the currently active transition realization as its old semantic state source and
> discards the obsolete pending steady form.

The temporary Front 8 analysis then proposed entering `GraphExecutor` once before
compilation and once after compilation. It considered moving `GraphJit` under
`GraphExecutor` because the application event-propagation rule permits entering each
child only once.

## Why it was superseded

`ProjectGraph` already owns the ordered logical revision transaction. If it requests
the transition A to B and later requests B to C, the execution path must preserve both
accepted transitions in that order. Another app module must not silently replace A to
B with A to C. It must not require `ProjectGraph` to guess that an accepted operation
will be skipped and retry compilation against a different predecessor.

The accepted design keeps both existing child edges:

```text
ProjectGraph -> GraphJit
ProjectGraph -> GraphExecutor
```

`ProjectGraph` invokes each child once for one rebuild. `GraphJit` produces the
executable successor for the accepted chain tail. `GraphExecutor` accepts that complete
generation only if its named predecessor is the current accepted chain tail. After
acceptance, `GraphExecutor` preserves and activates the transition in order. A
predecessor mismatch is an invariant violation. It is not a normal retry result.

# Ordered cutovers before failed-generation recovery

Status is historical. Preserved 2026-11-17 when the owner clarified that a
terminal preparation error can reject an executable generation before
activation while preserving its configured edits. See the current
[ordered cutover contract](../project_graph_application_architecture.md#recovery-from-terminal-preparation-failure).

## Project Graph Application Architecture

> `ProjectGraph` advances its executable predecessor only after `GraphExecutor` accepts
> the complete generation. If it requests A to B and later requests B to C,
> `GraphExecutor` preserves both transitions and activates A to B before B to C. An
> accepted transition is not replaceable by a later request. Failure before acceptance
> leaves the accepted chain tail unchanged. Failure while performing an accepted
> cutover is a fatal runtime error because continuing would violate the application
> transaction that `ProjectGraph` already completed.

> Each application-produced cutover is compiled from the accepted predecessor's steady
> realization. If that predecessor is still running its transition root, the successor
> remains queued until the predecessor reaches steady state. This makes acceptance
> durable: a transition-phase expiry cannot invalidate an accepted cutover or require
> recompilation.

## Realtime / Background Execution, Queues, And Capacity

> Preparing background-storage migration is itself submitted synchronously to the
> background actor. The staging caller may own the not-yet-published successor object, but
> it never inspects or prepares migration from actor-owned predecessor `NodeStorage`
> concurrently with evaluation. Any preparation failure is therefore still reported and
> the successor discarded before realtime can publish the cutover.

> An accepted generation is not active merely because preparation completed. However,
> acceptance commits its place in the transition chain. A later accepted generation is
> attached after it and cannot replace it. A generation that fails preparation is not
> accepted and can be reclaimed off realtime.

## Runtime Performance Roadmap

> The predecessor comes from the ordered executable chain owned by `ProjectGraph` and
> accepted by `GraphExecutor`. Once A to B is accepted, a later B to C request cannot
> replace it with A to C. `GraphExecutor` validates that each submitted predecessor is
> the accepted chain tail and preserves every accepted cutover in order.

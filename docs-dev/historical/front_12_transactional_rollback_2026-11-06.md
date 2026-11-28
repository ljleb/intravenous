# Front 12 Transactional Construction Rollback

_Historical status: non-normative. This file preserves the transactional-rollback
contract of the scoped-builder direction and the per-request failure isolation of
`NodeInstances`, as they stood before the accepted design change of 2026-11-06._

_Sources:
[Scoped GraphBuilder And SubgraphClosure Direction](../scoped_graph_builder_and_subgraph_closure_direction.md)
(invariant 10, the `ScopeFrame` sketch, the section "Same-session construction
requires transactional rollback", migration step 3, and one completion criterion) and
[Node Definitions And Instances Direction](../node_definitions_and_instances_direction.md)
(two statements about per-instance failure diagnostics). Preserved 2026-11-06, after
the Front 12 implementation, including its journaled rollback (F12-1), had passed the
repository owner's complete test suite._

_Reason for replacement: rollback existed to keep one failed request from preventing
its siblings from being realized in the same session. That isolation forced a journal
of in-place mutations, construction transactions, scope abandonment, and the defects
and open questions F12-10 through F12-13 recorded under Front 12 in
[implementation_fronts_direction.md](../implementation_fronts_direction.md). It also
made catching a failed construction inside module code an implicitly supported
pattern, which no document had specified. The human co-author decided that a
construction failure, and any invalid project node configuration, stops the whole
configuration pass instead. The pass's `BuilderSession` is discarded and the previous
executable generation keeps running, as it already does for `GraphJit` compile
failures. Nothing is undone in place, so no rollback exists._

_Current replacement: the section "Construction failure stops the configuration
pass" of
[Scoped GraphBuilder And SubgraphClosure Direction](../scoped_graph_builder_and_subgraph_closure_direction.md),
and the failure statements of
[Node Definitions And Instances Direction](../node_definitions_and_instances_direction.md)._

## Preserved invariant 10

> 10. Construction failure is transactional. Same-session nested construction
>     requires real rollback rather than merely abandoning a scope object.

## Preserved `ScopeFrame` sketch member

> ```cpp
>     SessionCheckpoint rollback_checkpoint;
> ```

## Preserved section text

> ## Same-session construction requires transactional rollback
>
> Independent child sessions currently provide an implicit failure boundary: a
> failed child can be destroyed without contaminating its parent graph. Direct
> same-session construction removes that accidental safety mechanism.
>
> Every opened construction scope therefore needs a real session checkpoint.
> Rollback must restore every append-only or scope-mutated structure affected by
> construction, including at least:
>
> ```text
> node-bundle count/state
> sample connection state
> event connection state
> sample-expression table
> event-expression table
> virtual-node/introspection state
> annotations/source state
> public/scope boundary state
> configuration allocations/lifetimes
> package/definition-use bookkeeping where transaction-local
> other future scope-local tables
> ```
>
> If an inner scope connects to an existing outer node and later throws, rollback
> must remove that connection too.
>
> Tiled module construction needs an outer transaction checkpoint around the whole
> tile set:
>
> ```text
> checkpoint
>     construct member 0
>     construct member 1
>     ...
>     validate compatible interfaces
> commit
> ```
>
> Any member/configuration/interface failure rolls back the complete tiled attempt.
> This preserves the present no-partial-tile guarantee without requiring independent
> child sessions.

## Preserved migration step 3

> 3. add the session-owned LIFO construction-scope stack and real transactional
>    checkpoints/rollback;

## Preserved completion criterion

> - nested construction rollback leaves no partial nodes, connections,
>   expressions, annotations, or configuration allocations;

## Preserved `NodeInstances` statements

From the list of completed checkpoint behavior:

> - cycle detection and batched per-instance diagnostics;

From the list of what one `NodeInstances` invocation does:

> 7. return batched diagnostics for unresolved/failed requests.

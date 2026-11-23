# Front 12 SubgraphClosure With A Live GraphBuilder Alternative

_Historical status: non-normative. This file preserves the `SubgraphClosure` section
and migration-outline item of the scoped-builder direction as they stood before the
accepted design change of 2026-11-06._

_Source: [Scoped GraphBuilder And SubgraphClosure Direction](../scoped_graph_builder_and_subgraph_closure_direction.md),
section "One SubgraphClosure surface, three materially different paths" and item 5 of
"Migration outline after the sequencing gate", preserved 2026-11-06 after the Front 12
implementation had compiled and passed the repository owner's complete test suite._

_Reason for replacement: a nested live `GraphBuilder` is not a transportable closure
value. While its scope is open it is the session's current scope, so every node
constructed anywhere in the session belongs to it. Passed into another node's
construction, that node's own scope would open inside it and could not then finalize
its own enclosing scope. It is usable only as an immediate, one-shot finalization at
the level where it is open, whereas the frozen and callable alternatives are reusable
values meant to be passed through module scopes as construction arguments. The
co-designers decided that in-place finalization is a separate `g.subgraph` overload
and that `SubgraphClosure` has two alternatives._

_Current replacement: the section "One subgraph concept: two closure alternatives and
in-place scope finalization" of
[Scoped GraphBuilder And SubgraphClosure Direction](../scoped_graph_builder_and_subgraph_closure_direction.md)._

## Preserved section text

> ## One SubgraphClosure surface, three materially different paths
>
> The source-level subgraph API should converge on one semantic overload:
>
> ```cpp
> NodeRef GraphBuilder::subgraph(SubgraphClosure /* cv/ref form TBD */);
> ```
>
> `SubgraphClosure` is a variant-backed type with implicit construction from three
> semantic alternatives:
>
> ```text
> ConfiguredGraph
> GraphBuilder
> ErasedSubgraphFunction   // callable with SubgraphBuilder&
> ```
>
> The exact ownership wrappers/cv-ref signature may be selected during
> implementation so that move-only closures are supported and reusable alternatives
> can be invoked repeatedly. The important contract is the three alternatives and
> one source-level subgraph concept.
>
> Do **not** normalize all three alternatives to a child `GraphBuilder` merely for
> implementation uniformity. Each path has distinct efficiency/correctness needs.
>
> ### ConfiguredGraph alternative
>
> A `ConfiguredGraph` is already closed reusable graph data. `g.subgraph(frozen)`
> should import it directly through the configured-graph component importer and
> handle translation machinery.
>
> Do not rebuild a temporary `GraphBuilder` from the frozen graph.
>
> Repeated placement is naturally valid:
>
> ```text
> g.subgraph(configured)
> g.subgraph(configured)
> g.subgraph(configured)
> ```
>
> Each placement creates fresh live bundles with an explicit local-to-session
> translation.
>
> ### Live GraphBuilder alternative
>
> A nested `GraphBuilder` represents state that already exists in the same
> `BuilderSession`. Attaching/finishing that subgraph closes/finalizes the existing
> scope **in place**. Its bundles are not copied and are not removed/reintroduced.
>
> This alternative is effectively one-shot as a scope-finalization operation: once
> the represented live scope is closed, that same live scope cannot be attached a
> second time. This does not mean its graph contents are consumed; they remain in
> the owning session.
>
> The normal cache-miss module path should use this behavior internally.
>
> ### C++ construction-closure alternative
>
> The erased callable path opens a fresh scope, constructs a restricted
> `SubgraphBuilder` view for its boundary, invokes the callable, then finalizes the
> scope. Source syntax such as the current form remains valid:
>
> ```cpp
> g.subgraph([&](SubgraphBuilder& subgraph) {
>     auto x = g.node<"processor">();
>     subgraph.outputs(x);
> });
> ```
>
> The callback receives only boundary operations. Ordinary node creation occurs
> through a captured/live `GraphBuilder` view, and `session.current_scope()` places
> those nodes in the just-opened subgraph.
>
> The erased callable should not be forced through `std::function` if that would
> exclude useful move-only captures. A suitable move-only erased callable is
> preferred where the toolchain permits it.

## Preserved migration-outline item

> 5. add `SubgraphClosure` and the single semantic `g.subgraph(...)` surface while
>    retaining separate direct implementations for frozen, live, and callable
>    alternatives;

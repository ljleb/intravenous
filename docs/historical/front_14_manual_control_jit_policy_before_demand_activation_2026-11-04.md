# Front 14 Manual-Control JIT Policy Before Demand-Driven Activation

_Historical status: non-normative. This file preserves text from
`docs/node_presentation_and_manual_controls_direction.md`,
`docs/graph_jit_direction.md`, and `docs/implementation_fronts_direction.md` that the
accepted Front 14 simplification superseded on 2026-11-04._

_Reason for replacement: the prior text described control code as a family of variants
and tied the return to constant specialization to pointer release. The replacement uses
one active executable graph and one current replacement request. Hover requests
compilation but does not activate code. The replacement also removes live observation
from Front 14._

_Current replacement: see
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md),
[GraphJit Direction](../graph_jit_direction.md), and
[Outstanding Implementation Fronts](../implementation_fronts_direction.md)._

## Preserved dependency wording

> _Status: follow-on direction after the core GraphJit/executor path is stable.
> The scoped GraphBuilder identity work in
> [scoped_graph_builder_and_subgraph_closure_direction.md](../scoped_graph_builder_and_subgraph_closure_direction.md)
> provides the nested semantic identities used by source focus and presentation
> rebinding. Manual-control JIT specialization also depends on the final GraphJit /
> `RealtimeExecutor` realization-switching contract. This is not an instruction to pull
> UI/control work ahead of those runtime and builder milestones._

## Preserved interpolation lifecycle

> The intended lifecycle is:
>
> ```text
> settled:
>     exact value / specialization candidate
>
> scrub begins:
>     input becomes a dynamic Tick control
>
> for each audio callback while scrubbing:
>     latch the newest UI target
>     interpolate sample-by-sample from the previous effective value
>     to the latched target over the callback
>
> scrub ends:
>     stop interpolation
>     settle at the final exact manual value
> ```

## Preserved predictive specialization policy

> ## Predictive dynamic-JIT specialization
>
> Settled controls are candidates for constant specialization under GraphJit. A
> control that is about to become interactive needs a code variant in which the
> relevant manual input is dynamic.
>
> The initial policy is deliberately simple:
>
> 1. settled/manual inactive controls may be specialized as constants;
> 2. hovering/engaging a participating control requests a variant whose
>    `dynamic_inputs` set contains that input, so compilation can begin before the
>    first meaningful scrub movement;
> 3. another relevant hover/gesture updates the requested set and supersedes the
>    older request;
> 4. simultaneous gestures are represented as a set, not one optional active
>    knob:
>
>    ```text
>    dynamic_inputs = { A, B, ... }
>    ```
>
> 5. initially there is no recent-variant cache; compile/reuse policy can be
>    revisited only after profiling;
> 6. a compilation that cannot be cancelled inside LLVM carries exact
>    project-revision/request identity, and its result is discarded if stale;
> 7. on release, a settled constant-specialized variant is requested again;
> 8. until that settled variant is ready, the currently active dynamic variant
>    continues reading the final unchanged manual scalar.
>
> This is code specialization, not graph/state replacement.

## Preserved compatibility wording

> ## All compatible control variants share NodeLayout and state semantics
>
> Dynamic and constant-specialized variants for one structural graph realization
> must use the same canonical `NodeLayout` and `NodeStorage`. Switching between
> those variants is therefore code-only and requires no storage migration.
>
> The compatibility rule is stronger than identical offsets:
>
> > Code variants declared layout-compatible must preserve compatible persistent-
> > state semantics, not merely the same byte layout.
>
> A constant specialization cannot stop updating persistent state that a later
> switch-compatible dynamic variant expects to continue from. Any specialization
> that changes graph structure, state identity, initialization/lifecycle, or
> `NodeLayout` is a structural recompilation rather than a code-only control
> variant.

## Preserved GraphJit manual-control wording

> ### Manual-control dynamic-input variants
>
> The planned manual-control application of this mechanism is specified in
> [node_presentation_and_manual_controls_direction.md](../node_presentation_and_manual_controls_direction.md).
> A settled participating manual value is a constant-specialization candidate.
> Hover/gesture intent may request a code variant whose exact `dynamic_inputs` set
> contains the controls that must be read dynamically; simultaneous gestures form a
> set rather than one optional control. Stale LLVM results carry exact request/revision
> identity and are discarded when superseded. On release, a settled constant variant
> is requested again while the current dynamic variant may continue reading the final
> unchanged scalar until replacement is ready.
>
> All such variants are code-only variants of one structural realization: they share
> the same canonical `NodeLayout`/`NodeStorage` and must preserve compatible persistent-
> state semantics, not merely identical offsets. No recent-variant cache is required
> initially; add one only if later profiling justifies it.

## Preserved implementation order

> 1. finish GraphJit plus the realtime/background executors;
> 2. perform the planned large optimization/profiling iteration;
> 3. perform the scoped GraphBuilder/session-identity migration where required for
>    stable nested virtual identities;
> 4. establish presentation rebinding around authored/scoped semantic identities;
> 5. add persistent manual values/participation policy to project state;
> 6. add long-lived gesture contexts and scrub-only interpolation;
> 7. add predictive dynamic-input specialization on top of the settled GraphJit
>    value-specialization/`RealtimeExecutor` activation machinery;
> 8. profile before adding recent-variant caches or more elaborate hover policies.

## Preserved Front 14 ownership item

> - eventual live-observation capability/subscription APIs once concrete requirements are
>   settled.

The Front 13 coordination text also said:

> The live-observation/presentation telemetry API remains separate and may stay unresolved
> until front 14 needs concrete capabilities.

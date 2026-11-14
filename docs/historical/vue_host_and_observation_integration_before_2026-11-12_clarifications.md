# Vue Host And Observation Integration Before The 2026-11-12 Clarifications

_Status: historical, non-normative. These are the verbatim passages superseded
by a later documentation-only clarification on 2026-11-12. The current
presentation-host rules are in
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md),
with the observation contracts in
[Vue Output Observation And Query Direction](../vue_output_observation_direction.md)
and [Graph JIT Direction](../graph_jit_direction.md)._

The earlier documents left the registration-removal ordering imprecise and
used an overly broad shorthand for observable fields/ports. The passages below
are preserved to distinguish the former wording from the clarified destination.

## Superseded passage from `docs/node_presentation_and_manual_controls_direction.md`

````text
1. Define a frontend-owned presentation registry, generic Vue fallback, and the four
   display modes; remove the obsolete backend `NodePresentation` registration path
   when implementing the replacement Vue integration.
````

## Superseded passage from `docs/graph_jit_direction.md`

````text
### Planned observation-aware incremental realization

The Vue client may request observations of supported reflected `State` fields and
resolved port data. `GraphJit` should incorporate their union into the incremental
realization and place only the needed capture operations at legal production/lifetime
boundaries. An observation-only change may rebuild/replace the immutable root/capture
plan at a safe boundary without reconstructing node state or rescheduling unrelated
work. If observing an otherwise unavailable output genuinely requires production,
materialization, or specialization changes, perform only those necessary changes.
Do not retain per-node realtime observation checks when no observation requires them.

These are intended semantics, not a claim that the current GraphJit already provides
UI observation probes. Reuse Front 16 incremental compilation and unchanged
realization artifacts wherever compatible; benchmark probe-only edits, native-region
specialization interactions, and realtime overhead before fixing a particular
plan/overlay implementation. Viewport movement can legitimately request compilation,
with client overscan/hysteresis reducing churn. See
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
````

## Superseded passage from `docs/node_interaction_and_presentation_communication_direction.md`

````text
- generating the frontend TypeScript protocol surface;
````
